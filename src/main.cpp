#define BIKESHED_IMPLEMENTATION
#include "../third-party/bikeshed/src/bikeshed.h"

#include "../third-party/nadir/src/nadir.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#    include <Windows.h>
#endif


#define H_COMPONENT 0
#define S_COMPONENT 1
#define V_COMPONENT 2

#define R_COMPONENT 0
#define G_COMPONENT 1
#define B_COMPONENT 2

#define SET_COL(col, r, g, b) col[R_COMPONENT] = r;col[G_COMPONENT] = g;col[B_COMPONENT] = b

static void hsv2rgb(double h, double s, double v, uint8_t* col)
{
    if(col[S_COMPONENT] <= 0.0) {       // < is bogus, just shuts up warnings
        uint8_t temp = (uint8_t)(255 * v);
        SET_COL(col, temp, temp, temp);
    }
    double hh = h;
    if(hh >= 360.0) hh = 0.0;
    hh /= 60.0;
    long i = (long)hh;
    double ff = hh - i;
    double p = v * (1.0 - s);
    double q = v * (1.0 - (s * ff));
    double t = v * (1.0 - (s * (1.0 - ff)));

    switch(i) {
    case 0:
        SET_COL(col, (uint8_t)(255 * v), (uint8_t)(255 * t), (uint8_t)(255 * p));
        break;
    case 1:
        SET_COL(col, (uint8_t)(255 * q), (uint8_t)(255 * v), (uint8_t)(255 * p));
        break;
    case 2:
        SET_COL(col, (uint8_t)(255 * p), (uint8_t)(255 * v), (uint8_t)(255 * t));
        break;
    case 3:
        SET_COL(col, (uint8_t)(255 * p), (uint8_t)(255 * q), (uint8_t)(255 * v));
        break;
    case 4:
        SET_COL(col, (uint8_t)(255 * t), (uint8_t)(255 * p), (uint8_t)(255 * v));
        break;
    case 5:
    default:
        SET_COL(col, (uint8_t)(255 * v), (uint8_t)(255 * p), (uint8_t)(255 * q));
        break;
    }
}

static void map_color(uint32_t iter, uint32_t max_iter, double r, double c, uint8_t* col)
{
    double di=(double )(iter * 120. / max_iter);
    double zn;
    double hue;

    static const double escape_radius = log(2.0);

    zn = sqrt(r + c);
    hue = di + 1.0 - log(log(fabs(zn))) / escape_radius;  // 2 is escape radius
    hue = 0.95 + 20.0 * hue; // adjust to make it prettier
    // the hsv function expects values from 0 to 360
    while (hue > 360.0)
        hue -= 360.0;
    while (hue < 0.0)
        hue += 360.0;

    hue += 240.0;

    return hsv2rgb(hue, 0.8, 0.5 + (0.5 * iter / max_iter), col);
}

struct Work
{
    nadir::TAtomic32* m_SubmittedWorkCount;
    nadir::TAtomic32* m_ActiveWorkCount;
    double m_X1;
    double m_Y1;
    double m_X2;
    double m_Y2;
    uint32_t m_MaxIterationsForBlock;
    uint32_t m_MaxIterationsToStop;
    uint32_t m_Width;
    uint32_t m_Height;
    uint32_t m_StartX;
    uint32_t m_StartY;
    uint32_t m_EndX;
    uint32_t m_EndY;
    uint8_t* m_Output;
    uint32_t m_ScanlineWidth;
};

struct NadirLock
{
    Bikeshed_ReadyCallback m_ReadyCallback;
    NadirLock()
        : m_ReadyCallback { signal }
        , m_Lock(nadir::CreateLock(malloc(nadir::GetNonReentrantLockSize())))
        , m_ConditionVariable(nadir::CreateConditionVariable(malloc(nadir::GetConditionVariableSize()), m_Lock))
    {
    }
    ~NadirLock()
    {
        nadir::DeleteConditionVariable(m_ConditionVariable);
        free(m_ConditionVariable);
        nadir::DeleteNonReentrantLock(m_Lock);
        free(m_Lock);
    }
    static void signal(Bikeshed_ReadyCallback* primitive, uint32_t ready_count)
    {
        NadirLock* _this = (NadirLock*)primitive;
        if (ready_count > 1)
        {
            nadir::WakeAll(_this->m_ConditionVariable);
        }
        else if (ready_count > 0)
        {
            nadir::WakeOne(_this->m_ConditionVariable);
        }
    }
    nadir::HSpinLock          m_SpinLock;
    nadir::HNonReentrantLock  m_Lock;
    nadir::HConditionVariable m_ConditionVariable;
};

struct NodeWorker
{
    NodeWorker()
        : stop(0)
        , shed(0)
        , condition_variable(0)
        , thread(0)
    {
    }

    ~NodeWorker()
    {
    }

    bool CreateThread(Bikeshed in_shed, nadir::HConditionVariable in_condition_variable, nadir::TAtomic32* in_stop)
    {
        shed               = in_shed;
        stop               = in_stop;
        condition_variable = in_condition_variable;
        thread             = nadir::CreateThread(malloc(nadir::GetThreadSize()), NodeWorker::Execute, 0, this);
        return thread != 0;
    }

    void DisposeThread()
    {
        nadir::DeleteThread(thread);
        free(thread);
    }

    static int32_t Execute(void* context)
    {
        NodeWorker* _this = (NodeWorker*)context;

        Bikeshed_TaskID next_ready_task = 0;
        while (*_this->stop == 0)
        {
            if (next_ready_task != 0)
            {
                Bikeshed_ExecuteAndResolve(_this->shed, next_ready_task, &next_ready_task);
                continue;
            }
            if (!Bikeshed_ExecuteOne(_this->shed, &next_ready_task))
            {
                nadir::SleepConditionVariable(_this->condition_variable, 1000);
            }
        }
        return 0;
    }

    nadir::TAtomic32*           stop;
    Bikeshed                    shed;
    nadir::HConditionVariable   condition_variable;
    nadir::HThread              thread;
};

static nadir::TAtomic32 gFrameIndex = 0;
static long gPeakActiveCount = 0;


static void DoArea(Work* work, Bikeshed_TaskID task_id, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, void** sub_work_data, uint16_t* sub_work_count)
{
    uint32_t next_max_iterations = work->m_MaxIterationsForBlock * 2;
    if (next_max_iterations > work->m_MaxIterationsToStop)
    {
        next_max_iterations = work->m_MaxIterationsToStop;
    }
    double h = work->m_Y2 - work->m_Y1;
    double w = work->m_X2 - work->m_X1;
    double x_step_factor = w / work->m_Width;
    double y_step_factor = h / work->m_Height;
    for (uint32_t y = y1; y < y2; ++y)
    {
        uint8_t* line_start = &work->m_Output[work->m_ScanlineWidth * y];

        double m_y = work->m_Y1 + y * y_step_factor;
        for (uint32_t x = x1; x < x2; ++x)
        {
            double m_x = work->m_X1 + x * x_step_factor;

            double u = 0.0;
            double v = 0.0;
            double u2 = u * u;
            double v2 = v * v;
            uint32_t iter = 0;
            for (iter = 0; iter < work->m_MaxIterationsForBlock && (u2 + v2 < 4.0); iter++) {
                v = 2 * u * v + m_y;
                u = u2 - v2 + m_x;
                u2 = u * u;
                v2 = v * v;
            };

            uint8_t* pixel = &line_start[sizeof(uint8_t) * 3 * x];

            if (iter == work->m_MaxIterationsForBlock)
            {
                if (work->m_MaxIterationsForBlock != work->m_MaxIterationsToStop)
                {
                    uint8_t r = (uint8_t)(task_id & 0xff);
                    uint8_t g = (uint8_t)((task_id >> 8) & 0xff);
                    uint8_t b = 255u - (uint8_t)((task_id >> 16) & 0xff);
                    for (y = y1; y < y2; ++y)
                    {
                        line_start = &work->m_Output[work->m_ScanlineWidth * y];

                        for (x = x1; x < x2; ++x)
                        {
                            pixel = (uint8_t*)& line_start[sizeof(uint8_t) * 3 * x];
                            SET_COL(pixel, r, g, b);
                        }
                    }

                    Work* subwork = (Work*)malloc(sizeof(Work));
                    if (subwork == 0)
                    {
                        printf("Failed to create subwork\n");
                        return;
                    }
                    *subwork = *work;
                    subwork->m_StartX = x1;
                    subwork->m_StartY = y1;
                    subwork->m_EndX = x2;
                    subwork->m_EndY = y2;
                    subwork->m_MaxIterationsForBlock = next_max_iterations;
                    sub_work_data[(*sub_work_count)++] = subwork;

                    return;
                }
                else
                {
                    SET_COL(pixel, 0u, 0u, 0u);
                }
            }
            else
            {
                map_color(iter, work->m_MaxIterationsToStop, v2, u2, pixel);
            }
        }
    }
}

static Bikeshed_TaskResult Calculate(Bikeshed shed, Bikeshed_TaskID task_id, void* context_data)
{
    Work* work = (Work*)context_data;

//    for (uint32_t y = work->m_StartY; y < work->m_EndY; ++y)
//    {
//        uint8_t* line_start = &work->m_Output[work->m_ScanlineWidth * y];
//
//        for (uint32_t x = work->m_StartX; x < work->m_EndX; ++x)
//        {
//            uint8_t* pixel = (uint8_t*)& line_start[sizeof(uint8_t) * 3 * x];
//            SET_COL(pixel, (uint8_t)(task_id & 0xff), (uint8_t)((task_id >> 8) & 0xff), (uint8_t)((task_id >> 16) & 0xff));
//        }
//    }

    void* sub_work_data[4];
    uint16_t sub_work_count = 0;
    uint32_t width = work->m_EndX - work->m_StartX;
    uint32_t height = work->m_EndY - work->m_StartY;
    if (width < 4 || height < 4)
    {
        work->m_MaxIterationsForBlock = work->m_MaxIterationsToStop;
        DoArea(work, task_id, work->m_StartX, work->m_StartY, work->m_EndX, work->m_EndY, sub_work_data, &sub_work_count);
    }
    else
    {
        uint32_t sub_width = (width + 1) / 2;
        uint32_t sub_height = (height + 1) / 2;

        DoArea(work, task_id, work->m_StartX, work->m_StartY, work->m_StartX + sub_width, work->m_StartY + sub_height, sub_work_data, &sub_work_count);
        DoArea(work, task_id, work->m_StartX + sub_width, work->m_StartY, work->m_EndX, work->m_StartY + sub_height, sub_work_data, &sub_work_count);
        DoArea(work, task_id, work->m_StartX, work->m_StartY + sub_height, work->m_StartX + sub_width, work->m_EndY, sub_work_data, &sub_work_count);
        DoArea(work, task_id, work->m_StartX + sub_width, work->m_StartY + sub_height, work->m_EndX, work->m_EndY, sub_work_data, &sub_work_count);
    }

    if (sub_work_count > 0)
    {
        Bikeshed_TaskID sub_tasks[4];
        BikeShed_TaskFunc funcs[4] = {Calculate, Calculate, Calculate, Calculate};
        if (Bikeshed_CreateTasks(shed, sub_work_count, funcs, sub_work_data, sub_tasks))
        {
            Bikeshed_ReadyTasks(shed, sub_work_count, sub_tasks);
            nadir::AtomicAdd32(work->m_SubmittedWorkCount, sub_work_count);
            long active_count = nadir::AtomicAdd32(work->m_ActiveWorkCount, sub_work_count);
            if (active_count > gPeakActiveCount)
            {
                gPeakActiveCount = active_count;
            }
        }
        else
        {
            printf("Failed to create jobs, currently %d active jobs!\n", (int)gPeakActiveCount);
        }
    }

    nadir::AtomicAdd32(work->m_ActiveWorkCount, -1);
    free(work);

    return BIKESHED_TASK_RESULT_COMPLETE;
}

static void DivideWork(Bikeshed shed, Work* work)
{
    uint32_t width = work->m_EndX - work->m_StartX;
    uint32_t height = work->m_EndY - work->m_StartY;

    uint32_t sub_width = (width + 1) / 2;
    uint32_t sub_height = (height + 1) / 2;

    Work* sub_work_data[4];
    for (uint32_t i = 0; i < 4; ++i)
    {
        sub_work_data[i] = (Work*)malloc(sizeof(Work));
        *sub_work_data[i] = *work;
    }

    sub_work_data[0]->m_StartX = work->m_StartX;
    sub_work_data[0]->m_StartY = work->m_StartY;
    sub_work_data[0]->m_EndX = work->m_StartX + sub_width;
    sub_work_data[0]->m_EndY = work->m_StartY + sub_height;

    sub_work_data[1]->m_StartX = work->m_StartX + sub_width;
    sub_work_data[1]->m_StartY = work->m_StartY;
    sub_work_data[1]->m_EndX = work->m_EndX;
    sub_work_data[1]->m_EndY = work->m_StartY + sub_height;

    sub_work_data[2]->m_StartX = work->m_StartX;
    sub_work_data[2]->m_StartY = work->m_StartY + sub_height;
    sub_work_data[2]->m_EndX = work->m_StartX + sub_width;
    sub_work_data[2]->m_EndY = work->m_EndY;

    sub_work_data[3]->m_StartX = work->m_StartX + sub_width;
    sub_work_data[3]->m_StartY = work->m_StartY + sub_height;
    sub_work_data[3]->m_EndX = work->m_EndX;
    sub_work_data[3]->m_EndY = work->m_EndY;

    Bikeshed_TaskID sub_tasks[4];
    BikeShed_TaskFunc funcs[4] = {Calculate, Calculate, Calculate, Calculate};
    if (Bikeshed_CreateTasks(shed, 4, funcs, (void**)sub_work_data, sub_tasks))
    {
        Bikeshed_ReadyTasks(shed, 4, sub_tasks);
        nadir::AtomicAdd32(work->m_SubmittedWorkCount, 4);
        long active_count = nadir::AtomicAdd32(work->m_ActiveWorkCount, 4);
        if (active_count > gPeakActiveCount)
        {
            gPeakActiveCount = active_count;
        }
    }
    else
    {
        printf("Failed to create jobs, currently %d active jobs!\n", (int)gPeakActiveCount);
    }
}


static uint64_t GetTick()
{
#if defined(_WIN32)
    LARGE_INTEGER tick;
    QueryPerformanceCounter(&tick);
    return tick.QuadPart;
#else
    return 0;
#endif
}

static uint64_t GetTicksPerSecond()
{
    static uint64_t gTicksPerSecond = 0;
    if (gTicksPerSecond == 0)
    {
#if defined(_WIN32)
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        gTicksPerSecond = freq.QuadPart;
        return gTicksPerSecond;
#else
        gTicksPerSecond = 1;
#endif
    }
    return gTicksPerSecond;
}

int main(int argc, char** argv)
{
    NadirLock sync_primitive;
    nadir::TAtomic32 stop = 0;

    uint32_t width = 1920u;
    uint32_t height = 1080u;

    double x = -1.398995;//-1.2;// -1.398995;
    double y = 0.001901;//0.0;// 0.001901;
    double w = 0.0000035;//3.0;// 0.000005;
    uint32_t i = 262144 * 2;
    uint32_t thread_count = 8;

    uint32_t ms_per_frame = 16;

    if (argc == 9)
    {
        sscanf(argv[1], "%u", &width);
        sscanf(argv[2], "%u", &height);

        sscanf(argv[3], "%lf", &x);
        sscanf(argv[4], "%lf", &y);
        sscanf(argv[5], "%lf", &w);
        sscanf(argv[6], "%u", &i);

        sscanf(argv[7], "%u", &thread_count);

        sscanf(argv[8], "%u", &ms_per_frame);
    }

    printf("Generating: Image(%u,%u) Pos(%.6lf, %.6lf), Size(%.8lf) Iterations(%u), Threads(%u)\n", width, height, x, y, w, i, thread_count);

    Bikeshed shed = Bikeshed_Create(malloc(Bikeshed_GetSize(65535, 65535)), 65535, 65535, &sync_primitive.m_ReadyCallback);
    if (shed == 0)
    {
        printf("Failed to create shed\n");
        return -1;
    }

    uint32_t scanline_width = sizeof(uint8_t) * 3 * width;
    uint8_t* output = (uint8_t*)malloc(scanline_width * height);
    memset(output, 32u, scanline_width * height);

    double x_width = (w * width) / height / 2.0;
    double y_height = w / 2.0;

    Work* work = (Work*)malloc(sizeof(Work));
    if (work == 0)
    {
        printf("Failed to allocate initial work\n");
        return -1;
    }

    nadir::TAtomic32 submitted_work_count = 0;
    nadir::TAtomic32 active_work_count = 0;

    work->m_SubmittedWorkCount = &submitted_work_count;
    work->m_ActiveWorkCount = &active_work_count;
    work->m_X1 = x - x_width;
    work->m_Y1 = y - y_height;
    work->m_X2 = x + x_width;
    work->m_Y2 = y + y_height;
    work->m_MaxIterationsForBlock = 256;
    work->m_MaxIterationsToStop = i;
    work->m_Width = width;
    work->m_Height = height;
    work->m_StartX = 0;
    work->m_StartY = 0;
    work->m_EndX = work->m_Width;
    work->m_EndY = work->m_Height;
    work->m_ScanlineWidth = scanline_width;
    work->m_Output = output;

    NodeWorker* thread_context = new NodeWorker[thread_count];
    for (uint32_t t = 0; t < thread_count; ++t)
    {
        if (!thread_context[t].CreateThread(shed, sync_primitive.m_ConditionVariable, &stop))
        {
            printf("Failed to create thread %u", t + 1);
            return -1;
        }
    }

    uint64_t start_tick = GetTick();

    DivideWork(shed, work);

    free(work);
    work = 0;

    while (active_work_count > 0)
    {
        if (ms_per_frame != 0)
        {
            char filename[128];
            sprintf(filename, "overbroth-%05ld.ppm", nadir::AtomicAdd32(&gFrameIndex, 1));
            FILE* fp = fopen(filename, "wb"); /* b - binary mode */
            if (fp)
            {
                (void)fprintf(fp, "P6\n%d %d\n255\n", width, height);
                (void)fwrite(output, 3, width * height, fp);
                (void)fclose(fp);
            }
            nadir::Sleep(ms_per_frame * 1000);    // 60 fps, roughly
        }
        else
        {
            nadir::Sleep(16000);
        }
    }

    uint64_t end_tick = GetTick();

    uint64_t elapsed_tick = end_tick - start_tick;
    uint64_t elapsed_ms = (elapsed_tick * 1000) / GetTicksPerSecond();

    nadir::AtomicAdd32(&stop, 1);
    nadir::WakeAll(sync_primitive.m_ConditionVariable);

    for (uint32_t t = 0; t < thread_count; ++t)
    {
        nadir::JoinThread(thread_context[t].thread, nadir::TIMEOUT_INFINITE);
        thread_context[t].DisposeThread();
    }

    delete [] thread_context;

    free(shed);

    char filename[128];
    sprintf(filename, "overbroth-%05ld.ppm", nadir::AtomicAdd32(&gFrameIndex, 1));
    FILE* fp = fopen(filename, "wb"); /* b - binary mode */
    if (fp)
    {
        (void)fprintf(fp, "P6\n%d %d\n255\n", width, height);
        (void)fwrite(output, 3, width * height, fp);
        (void)fclose(fp);
    }

    free(output);

    printf("Threads: %u, Jobs submitted: %d. Peak active jobs: %d, Elapsed time: %.3f s", thread_count, (int)submitted_work_count, (int)gPeakActiveCount, (float)(elapsed_ms / 1000.f));

    return 0;
}

// ..\..\..\..\ffmpeg-20190409-0a347ff-win64-static\bin\ffmpeg.exe -r 60 -f image2 -s 1920x1080 -i overbroth-%05d.ppm -vcodec libx264 -crf 25  -pix_fmt yuv420p overbroth.mp4
