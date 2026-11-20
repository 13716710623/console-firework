/*
 * firework.c — 多烟花并发控制台动画（R2）
 * 平台：Windows(MinGW) / POSIX，C99 标准库 + 条件编译平台 API
 * 构建：gcc firework.c -o firework -Wall -Wextra（零警告）
 */

/* POSIX 特性宏：确保 nanosleep / ioctl 可见（仅非 Windows） */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ===== 平台头文件（条件编译） ===== */
#ifdef _WIN32
   #include <windows.h>
  #include <conio.h>            /* _kbhit 非阻塞按键检测 */
#else
   #include <unistd.h>
   #include <sys/ioctl.h>
  #include <sys/time.h>         /* gettimeofday 高精度计时 */
   #include <termios.h>
#endif

/* ===== 常量宏（禁止魔法数字） ===== */
#define MAX_PARTICLES   4096          /* 粒子数组容量 */
#define MAX_COLS        200           /* 画布最大列数 */
#define MAX_ROWS        60            /* 画布最大行数 */
#define DEFAULT_FPS     30            /* 默认帧率 */
#define FALLBACK_COLS   80            /* 读取失败时回退列数 */
#define FALLBACK_ROWS   24            /* 读取失败时回退行数 */
#define RAY_DIRS        64            /* R4: 64 uniform directions per explosion */
/* R5：爆炸类型与粒子类型 */
enum { EX_PEONY = 0, EX_WILLOW = 1, EX_RING = 2 };
enum { KIND_NORMAL = 0, KIND_SPARK = 1, KIND_TWINKLE = 2 };
#define WILLOW_DIRS            32     /* 柳条：方向更少，垂感更强 */
#define WILLOW_GRAVITY_SCALE   1.10f  /* 柳条重力倍率：下坠更快 */
#define RING_SPEED_SCALE       1.05f  /* 环形：速度带更窄、更靠外 */
#define SPARK_MINI_COUNT       6      /* 火星二次爆裂的粒子数 */
#define RAY_LAYERS_MIN  4             /* R4: 每发烟花的层数下限（爆炸规模随机） */
#define RAY_LAYERS_MAX  8             /* R4: 每发烟花的层数上限（64×(3~6)=192~384 粒子） */
#define RESIZE_CHECK_FRAMES 30        /* R4: re-read console size every N frames */
#define MIN_LIFE        90            /* 粒子最短寿命（帧） */
#define MAX_LIFE        130           /* 粒子最长寿命（帧） */
#define ROCKET_DRAG     0.99f         /* 火箭上升阻力系数 */
#define ROCKET_VY_MIN   0.8f          /* 火箭最小初速 */
#define ROCKET_VY_MAX   1.5f          /* 火箭最大初速 */
#define ROCKET_STOP     0.05f         /* 火箭近乎停止的阈值 */
#define MAX_ROCKETS        6          /* 同时存在的最大烟花数 */
#define LAUNCH_INTERVAL_MIN 4         /* 发射间隔最小帧数 */
#define LAUNCH_INTERVAL_MAX 12        /* 发射间隔最大帧数 */
#define GRAVITY         0.014f         /* 粒子重力加速度 */
#define PSPEED_MAX      0.85f         /* 粒子最大初速 */
#define NUM_BRIGHT      6             /* 亮度等级数 */
#define FRAMEBUF_SIZE   (MAX_ROWS * (MAX_COLS + 1) + 32)

/* 亮度距离平方阈值（离爆炸中心越近越亮） */
#define BDIST0   2.25f                /* d < 1.5  -> '@' */
#define BDIST1   9.0f                 /* d < 3.0  -> 'O' */
#define BDIST2   25.0f                /* d < 5.0  -> 'o' */
#define BDIST3   49.0f                /* d < 7.0  -> '*' */
#define BDIST4   81.0f               /* d < 9.0  -> ',' */
/* d >= 12.0 -> '.' */

/* ===== 数据结构 ===== */

/* 粒子：爆炸后产生，带速度/重力/寿命/爆炸中心 */
typedef struct {
    float x, y;         /* 当前坐标（列, 行） */
    float vx, vy;       /* 速度 */
    int   life;         /* 剩余寿命（帧） */
    int   max_life;     /* 出生寿命（用于余烬变暗） */
    float cx, cy;       /* 爆炸中心坐标（用于亮度计算） */
    unsigned char kind; /* 粒子类型：普通 / 火星（消亡时二次爆裂）/ 闪烁 */
    unsigned char dim;  /* 整体压暗档数（柳条用） */
} Particle;

/* 火箭：上升阶段，到达目标高度后爆炸 */
typedef struct {
    float x, y;       /* 当前坐标 */
    float vx, vy;     /* 速度 */
    float target_y;   /* 目标高度（行号，越小越高） */
    int   active;     /* 是否在飞行中 */
} Rocket;

/* ===== 全局静态数组 ===== */
static Particle g_particles[MAX_PARTICLES];
static int      g_particle_count = 0;
static Rocket   g_rockets[MAX_ROCKETS]; /* 烟箭数组（并发管理） */
static int      g_launch_cooldown = 0;  /* 距下次发射的帧数 */
static long     g_drop_count = 0;       /* 满上限丢弃的粒子计数 */
static int      g_ansi_mode = 0;        /* --ansi：强制 ANSI 光标归位 */
static int      g_selftest = 0;         /* --selftest：跑内置自检后退出 */
static int      g_help = 0;             /* --help / -h：打印用法后退出 */
static int      g_force_cols = 0;       /* --size WxH：录制/测试时强制画布宽 */
static int      g_force_rows = 0;       /* --size WxH：录制/测试时强制画布高 */
static char     g_canvas[MAX_ROWS][MAX_COLS];
static int      g_bright[MAX_ROWS][MAX_COLS];
static char     g_framebuf[FRAMEBUF_SIZE];

#ifdef _WIN32
static CONSOLE_CURSOR_INFO g_orig_cursor; /* 进入前光标信息，用于恢复 */
#else
static struct termios g_orig_termios;     /* 进入前终端属性，用于恢复 */
static int g_term_raw = 0;                /* 是否已切到非阻塞模式 */
#endif

/* 亮度字符表：索引越小越亮 @ O o * , . */
static const char BRIGHT_CHARS[NUM_BRIGHT] = {'@', 'O', 'o', '*', ',', '.'};

/* 单位圆方向表：64 等分，避免引入 math.h / libm 依赖。
 * 索引 0..63 对应 0~2π，爆炸粒子按 i 均匀取方向，形成放射状而非随机散点。 */
#define UNIT_STEPS 64
static const float UNIT_X[UNIT_STEPS] = {
    1.0000f, 0.9952f, 0.9808f, 0.9569f, 0.9239f, 0.8819f, 0.8315f, 0.7730f,
    0.7071f, 0.6344f, 0.5556f, 0.4714f, 0.3827f, 0.2903f, 0.1951f, 0.0980f,
    0.0000f, -0.0980f, -0.1951f, -0.2903f, -0.3827f, -0.4714f, -0.5556f, -0.6344f,
    -0.7071f, -0.7730f, -0.8315f, -0.8819f, -0.9239f, -0.9569f, -0.9808f, -0.9952f,
    -1.0000f, -0.9952f, -0.9808f, -0.9569f, -0.9239f, -0.8819f, -0.8315f, -0.7730f,
    -0.7071f, -0.6344f, -0.5556f, -0.4714f, -0.3827f, -0.2903f, -0.1951f, -0.0980f,
    -0.0000f, 0.0980f, 0.1951f, 0.2903f, 0.3827f, 0.4714f, 0.5556f, 0.6344f,
    0.7071f, 0.7730f, 0.8315f, 0.8819f, 0.9239f, 0.9569f, 0.9808f, 0.9952f,
};
static const float UNIT_Y[UNIT_STEPS] = {
    0.0000f, 0.0980f, 0.1951f, 0.2903f, 0.3827f, 0.4714f, 0.5556f, 0.6344f,
    0.7071f, 0.7730f, 0.8315f, 0.8819f, 0.9239f, 0.9569f, 0.9808f, 0.9952f,
    1.0000f, 0.9952f, 0.9808f, 0.9569f, 0.9239f, 0.8819f, 0.8315f, 0.7730f,
    0.7071f, 0.6344f, 0.5556f, 0.4714f, 0.3827f, 0.2903f, 0.1951f, 0.0980f,
    0.0000f, -0.0980f, -0.1951f, -0.2903f, -0.3827f, -0.4714f, -0.5556f, -0.6344f,
    -0.7071f, -0.7730f, -0.8315f, -0.8819f, -0.9239f, -0.9569f, -0.9808f, -0.9952f,
    -1.0000f, -0.9952f, -0.9808f, -0.9569f, -0.9239f, -0.8819f, -0.8315f, -0.7730f,
    -0.7071f, -0.6344f, -0.5556f, -0.4714f, -0.3827f, -0.2903f, -0.1951f, -0.0980f,
};

/* ===== 平台抽象函数 ===== */

/*
 * 获取控制台实际尺寸。
 * 参数：cols/rows 输出当前列数和行数。
 * 边界：读取失败时回退 80x24；过小时回退；上限不超过 MAX_COLS/MAX_ROWS。
 */
static void get_console_size(int *cols, int *rows)
{
    int got = 0, c = 0, r = 0;

#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            c = info.srWindow.Right - info.srWindow.Left + 1;
            r = info.srWindow.Bottom - info.srWindow.Top + 1;
            got = 1;
        }
    }
#else
    {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            c = ws.ws_col;
            r = ws.ws_row;
            got = 1;
        }
    }
#endif

    if (!got) {
        if (*cols < 10) *cols = FALLBACK_COLS;
        if (*rows < 5)  *rows = FALLBACK_ROWS;
        return;
    }
    if (c < 10)        c = FALLBACK_COLS;
    if (r < 5)         r = FALLBACK_ROWS;
    if (c > MAX_COLS)  c = MAX_COLS;
    if (r > MAX_ROWS)  r = MAX_ROWS;
    *cols = c;
    *rows = r;
}

/*
 * 跨平台毫秒级休眠，不做忙等。
 * 参数：ms - 休眠毫秒数（<=0 时直接返回）。
 */
static void platform_sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    /* Windows 的 Sleep 受默认 15.6ms 定时器粒度影响，Sleep(5) 往往睡 15ms，
     * 直接导致帧率抖动。这里改用可等待定时器（Win10 1803+ 支持高精度模式），
     * 只依赖 kernel32，不需要额外链接 winmm。 */
    static HANDLE timer = NULL;
    LARGE_INTEGER due;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    if (timer == NULL) {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (timer == NULL) {
            timer = CreateWaitableTimerW(NULL, TRUE, NULL);  /* 回退：普通高精度定时器 */
        }
    }
    due.QuadPart = -((LONGLONG)ms * 10000LL);   /* 相对时间，单位 100ns */
    if (timer != NULL && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep((DWORD)ms);                        /* 最终回退 */
    }
#else
    {
        struct timespec ts;
        ts.tv_sec  = (time_t)(ms / 1000);
         ts.tv_nsec = (long)((ms % 1000) * 1000000L);
         nanosleep(&ts, NULL);
     }
#endif
}

/*
  * 获取高精度当前时间（毫秒），用于帧率稳定计算。
  * Windows 用 QueryPerformanceCounter，POSIX 用 gettimeofday。
  */
static double get_time_ms(void)
{
#ifdef _WIN32
     static LARGE_INTEGER freq = {0};
     LARGE_INTEGER now;
     if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
     QueryPerformanceCounter(&now);
     return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
     struct timeval tv;
     gettimeofday(&tv, NULL);
     return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#endif
}

/*
  * 帧率稳定：按本帧实际耗时休眠剩余时间，渲染耗时计入，非固定 sleep。
  * 参数：start_ms - 本帧开始时刻；frame_interval - 目标帧时长(毫秒)。
  */
static void sync_frame(double start_ms, int frame_interval)
{
     double deadline = start_ms + (double)frame_interval;
     double remain = deadline - get_time_ms();

     /* Windows 默认定时器粒度约 15.6ms，Sleep(4) 常睡成 15ms，帧率会抖。
      * 这里用「粗睡 + 末段自旋」混合：先睡到剩余约 1.5ms，再用高精度计时自旋补齐。 */
     if (remain > 2.0) {
         platform_sleep_ms((int)(remain - 1.5));
     }
     while (get_time_ms() < deadline) {
         /* 末段自旋，最多约 2ms，换取稳定的帧间隔 */
     }
}

/*
  * 光标归位到左上角 (0,0)，用于双缓冲覆盖式渲染。
  * --ansi 模式统一走 ANSI \033[H（便于重定向抓帧测试）；
  * 否则 Windows 用 SetConsoleCursorPosition，POSIX 用 ANSI。
  */
static void cursor_home(void)
{
     if (g_ansi_mode) {
         fputs("\033[H", stdout);
         return;
     }
#ifdef _WIN32
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
        COORD pos = {0, 0};
        SetConsoleCursorPosition(h, pos);
    }
#else
     fputs("\033[H", stdout);
#endif
}

/*
  * 隐藏光标：Windows 用 SetConsoleCursorInfo，POSIX 用 ANSI \033[?25l。
  */
static void hide_cursor(void)
{
#ifdef _WIN32
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     CONSOLE_CURSOR_INFO ci;
     if (h != INVALID_HANDLE_VALUE && GetConsoleCursorInfo(h, &g_orig_cursor)) {
         ci = g_orig_cursor;
         ci.bVisible = FALSE;
         SetConsoleCursorInfo(h, &ci);
     }
#else
     fputs("\033[?25l", stdout);
     fflush(stdout);
#endif
}

/*
  * 显示光标：恢复进入循环前的光标状态。
  */
static void show_cursor(void)
{
#ifdef _WIN32
     HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
     if (h != INVALID_HANDLE_VALUE) {
         SetConsoleCursorInfo(h, &g_orig_cursor);
     }
#else
     fputs("\033[?25h", stdout);
     fflush(stdout);
#endif
}

/*
  * 初始化终端交互：POSIX 设置非阻塞读取（VMIN=0/VTIME=0）。
  * Windows 无需特殊设置，_kbhit 本身非阻塞。
  */
static void init_terminal(void)
{
#ifndef _WIN32
     struct termios t;
     if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
         t = g_orig_termios;
         t.c_lflag &= ~(ICANON | ECHO);
         t.c_cc[VMIN] = 0;
         t.c_cc[VTIME] = 0;
         tcsetattr(STDIN_FILENO, TCSANOW, &t);
         g_term_raw = 1;
     }
#endif
}

/*
  * 恢复终端原始属性（POSIX）；退出前调用。
  */
static void restore_terminal(void)
{
#ifndef _WIN32
     if (g_term_raw) {
         tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
         g_term_raw = 0;
     }
#endif
}

/*
  * 非阻塞检测按键：有按键返回 1，否则返回 0（绝不阻塞主循环）。
  */
static int check_key_pressed(void)
{
#ifdef _WIN32
     return _kbhit() ? 1 : 0;
#else
     char c;
     ssize_t n = read(STDIN_FILENO, &c, 1);
     return (n > 0) ? 1 : 0;
#endif
}

/*
 * 清屏：程序启动和退出时调用，避免残影。
 * Windows 用 FillConsoleOutputCharacter，POSIX 用 ANSI \033[2J。
 */
static void clear_screen(void)
{
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        DWORD written;
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            COORD origin = {0, 0};
            DWORD cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
            FillConsoleOutputCharacter(h, ' ', cells, origin, &written);
            FillConsoleOutputAttribute(h, info.wAttributes, cells, origin, &written);
            SetConsoleCursorPosition(h, origin);
        }
    }
#else
    fputs("\033[2J\033[H", stdout);
    fflush(stdout);
#endif
}

/* ===== 参数解析 ===== */

/*
 * 解析命令行参数 --frames / --fps / --seed。
 * 参数：argc/argv 来自 main；frames/fps/seed 为输出。
 * 默认：frames=0(无限)，fps=30，seed=0(后续用 time(NULL))。
 * 负 frames 视为 0。
 */
static void parse_args(int argc, char **argv, int *frames, int *fps,
                        unsigned *seed, int *ansi)
{
     int i;
     *frames = 0;
     *fps    = DEFAULT_FPS;
     *seed   = 0;
     *ansi   = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            *frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            *fps = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
             *seed = (unsigned)strtoul(argv[++i], NULL, 10);
         } else if (strcmp(argv[i], "--ansi") == 0) {
             *ansi = 1;
         } else if (strcmp(argv[i], "--selftest") == 0) {
             g_selftest = 1;
         } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
             g_help = 1;
         } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
             /* 形如 120x36：给录屏/自动化测试用，强制画布尺寸 */
             int w = 0, h = 0;
             if (sscanf(argv[++i], "%dx%d", &w, &h) == 2) {
                 g_force_cols = w;
                 g_force_rows = h;
             }
         }
    }
    if (*frames < 0) *frames = 0;
    if (*fps < 1)    *fps = DEFAULT_FPS;
}

/* ===== 亮度计算 ===== */

/*
 * 根据粒子到爆炸中心的距离平方计算亮度等级。
 * 参数：dx/dy - 粒子位置与爆炸中心的偏移量。
 * 返回：0~5，0 最亮(@)，5 最暗(.)。
 */
static int compute_brightness(float dx, float dy)
{
    float d2 = dx * dx + dy * dy;
    if (d2 < BDIST0) return 0;  /* '@' */
    if (d2 < BDIST1) return 1;  /* 'O' */
    if (d2 < BDIST2) return 2;  /* 'o' */
    if (d2 < BDIST3) return 3;  /* '*' */
    if (d2 < BDIST4) return 4;  /* ',' */
    return 5;                   /* '.' */
}

/* ===== 画布操作 ===== */

/*
 * 清空画布：整幅填充空格，亮度标记设为最高值(空白)。
 * 说明：按 MAX_COLS/MAX_ROWS 全量清空，避免 draw_pixel 判界范围大于可见区域时残留幽灵像素。
 */
static void clear_canvas(void)
{
    int y, x;
    for (y = 0; y < MAX_ROWS; y++) {
        for (x = 0; x < MAX_COLS; x++) {
            g_canvas[y][x] = ' ';
            g_bright[y][x] = NUM_BRIGHT;
        }
    }
}

/*
 * 向画布写入一个像素，带边界校验和亮度优先级。
 * 参数：x/y - 整数坐标；brightness - 亮度等级(0最亮)；ch - 字符。
 * 边界：x/y 越界时直接返回；更亮的像素覆盖更暗的。
 */
static void draw_pixel(int x, int y, int brightness, char ch)
{
    if (x < 0 || x >= MAX_COLS || y < 0 || y >= MAX_ROWS) return;
    if (brightness < g_bright[y][x]) {
        g_bright[y][x] = brightness;
        g_canvas[y][x] = ch;
    }
}

/* ===== 火箭与粒子逻辑 ===== */

/*
  * 初始化火箭：从底部随机位置以随机初速向上飞行。
  * 参数：r - 火箭结构体指针；cols/rows - 画布尺寸。
  */
static void init_rocket(Rocket *r, int cols, int rows)
{
     int launch_x = 1 + rand() % (cols - 2);   /* 底部全宽随机发射点（左右各留 1 格边距） */
     r->x        = (float)launch_x;
    r->y        = (float)(rows - 1);
    r->vx       = ((float)(rand() % 100) - 50.0f) / 200.0f;
    r->vy       = -(ROCKET_VY_MIN +
                    (float)(rand() % 100) / 100.0f * (ROCKET_VY_MAX - ROCKET_VY_MIN));
     r->target_y = (float)(rows / 5 + rand() % (rows / 3 + 1));
     r->active   = 1;
}

/*
  * 发射火箭：在烟箭数组中寻找空闲槽位发射，无空位则跳过。
  * 参数：cols/rows - 画布尺寸。
  */
static void launch_rocket(int cols, int rows)
{
     int i;
     for (i = 0; i < MAX_ROCKETS; i++) {
         if (!g_rockets[i].active) {
             init_rocket(&g_rockets[i], cols, rows);
             return;
         }
     }
}

/*
 * 随机选择本次爆炸的类型：牡丹 50%、柳条 30%、环形 20%。
 * 参数：无。
 * 返回：EX_PEONY / EX_WILLOW / EX_RING。
 */
static int choose_explosion_type(void)
{
    int r = rand() % 10;
    if (r < 5) return EX_PEONY;
    if (r < 8) return EX_WILLOW;
    return EX_RING;
}

/*
 * 追加一个粒子；粒子池满时丢弃并计数。
 * 参数：cx/cy - 爆心；x/y - 出生位置；vx/vy - 初速；life - 寿命；
 *       kind - 粒子类型；dim - 压暗档数。
 * 返回：1 成功，0 满池丢弃。
 */
static int spawn_particle(float cx, float cy, float x, float y,
                          float vx, float vy, int life,
                          unsigned char kind, unsigned char dim)
{
    Particle *q;
    if (g_particle_count >= MAX_PARTICLES) {
        g_drop_count++;
        return 0;
    }
    q = &g_particles[g_particle_count++];
    q->x = x;      q->y = y;
    q->vx = vx;    q->vy = vy;
    q->cx = cx;    q->cy = cy;
    q->life = life;
    q->max_life = life;
    q->kind = kind;
    q->dim = dim;
    return 1;
}

/*
 * 火星的二次爆裂：在粒子消亡处再炸出几颗小火星。
 * 参数：x/y - 消亡位置（调用方已保证在画布内）。
 */
static void mini_burst(float x, float y)
{
    int i;
    for (i = 0; i < SPARK_MINI_COUNT; i++) {
        int dir = rand() % UNIT_STEPS;
        float sp = 0.25f + (float)(rand() % 100) / 100.0f * 0.35f;
        spawn_particle(x, y, x, y,
                       UNIT_X[dir] * sp,
                       UNIT_Y[dir] * sp * 0.6f,
                       25 + rand() % 15, KIND_NORMAL, 1);
    }
}

/*
 * 爆炸：按类型生成粒子。
 * 参数：type - 爆炸类型；cx/cy - 爆心。
 * 边界：总粒子数受 MAX_PARTICLES 限制，满池丢弃并计数。
 */
static void explode(int type, float cx, float cy)
{
    int base_dir = rand() % RAY_DIRS;
    int dirs, layers, k, j;
    float max_speed, vy_scale;
    int life_min, life_max, dim, spark_pct, twinkle_pct;

    if (cy < 1.0f) cy = 1.0f;

    switch (type) {
    case EX_WILLOW:
        dirs = WILLOW_DIRS; layers = 10 + rand() % 7;
        max_speed = PSPEED_MAX * 0.50f; vy_scale = 0.55f;
        life_min = 140; life_max = 200;
        dim = 1; spark_pct = 0; twinkle_pct = 0;
        break;
    case EX_RING:
        dirs = RAY_DIRS; layers = 2 + rand() % 2;
        max_speed = PSPEED_MAX * RING_SPEED_SCALE; vy_scale = 0.55f;
        life_min = 60; life_max = 95;
        dim = 0; spark_pct = 0; twinkle_pct = 25;
        break;
    default:
        dirs = RAY_DIRS;
        layers = RAY_LAYERS_MIN + rand() % (RAY_LAYERS_MAX - RAY_LAYERS_MIN + 1);
        max_speed = PSPEED_MAX; vy_scale = 0.55f;
        life_min = MIN_LIFE; life_max = MAX_LIFE;
        dim = 0; spark_pct = 12; twinkle_pct = 12;
        break;
    }

    /* 爆心闪光：几帧的高亮核心，模拟爆炸瞬间的强光 */
    for (k = 0; k < 14; k++) {
        int fd = rand() % UNIT_STEPS;
        float fsp = 0.02f + (float)(rand() % 100) / 3000.0f;
        (void)spawn_particle(cx, cy, cx, cy, UNIT_X[fd] * fsp, UNIT_Y[fd] * fsp,
                             8 + rand() % 9, KIND_NORMAL, 0);
    }
    for (k = 0; k < dirs; k++) {
        int dir = (base_dir + k * RAY_DIRS / dirs) % RAY_DIRS;
        for (j = 0; j < layers; j++) {
            float speed, f;
            int life;
            unsigned char kind = KIND_NORMAL;

            if (type == EX_RING) {
                f = 0.90f + 0.10f * (float)j / (float)layers;   /* 只有外圈，形成空心环 */
            } else {
                f = 0.30f + 0.70f * (float)j / (float)layers;   /* 层数越大越靠外 */
            }
            speed = max_speed * f;
            if (type == EX_RING) {
                speed *= 0.96f + (float)(rand() % 100) / 1250.0f;   /* 环形加抖动，减轻网格方框感 */
            }
            life = life_min + rand() % (life_max - life_min + 1);

            if (spark_pct > 0 && (rand() % 100) < spark_pct) {
                kind = KIND_SPARK;
            } else if (twinkle_pct > 0 && (rand() % 100) < twinkle_pct) {
                kind = KIND_TWINKLE;
            }

            (void)0;
            if (!spawn_particle(cx, cy, cx, cy,
                                UNIT_X[dir] * speed,
                                UNIT_Y[dir] * speed * vy_scale,
                                life, kind, (unsigned char)dim)) {
                return;   /* 池满，整发提前结束 */
            }
        }
    }
}

/*
 * 更新火箭：移动、施加阻力减速、检测爆炸条件。
 * 参数：r - 火箭；cols/rows - 画布尺寸。
 * 到达目标高度、速度趋零、或飞出上边界 -> 爆炸；
 * 飞出左右/下边界 -> 直接消失。
 */
static void update_rocket(Rocket *r, int cols, int rows)
{
    if (!r->active) return;
    r->x += r->vx;
    r->y += r->vy;
    r->vy *= ROCKET_DRAG;
    /* 上升烟迹：每帧抛一颗短命暗火星，形成连续的发射轨迹 */
    if (r->y > 0.0f && r->y < (float)rows && r->x > 0.0f && r->x < (float)cols) {
        (void)spawn_particle(r->x, r->y, r->x, r->y + 1.0f,
                             ((float)(rand() % 100) - 50.0f) / 400.0f,
                             0.04f + (float)(rand() % 100) / 1500.0f,
                             10 + rand() % 10, KIND_NORMAL, 2);
    }
    /* 到达目标高度或速度趋零或飞出顶部 -> 爆炸 */
    if (r->y <= r->target_y || r->vy >= -ROCKET_STOP || r->y < 0.0f) {
        r->active = 0;
        explode(choose_explosion_type(), r->x, r->y);
        return;
    }
    /* 飞出左右或下边界 -> 直接消失 */
    if (r->x < 0.0f || r->x >= (float)cols || r->y >= (float)rows) {
        r->active = 0;
    }
}

/*
 * 更新所有粒子：移动、施加重力、递减寿命、紧凑删除。
 * 参数：cols/rows - 画布尺寸。
 * 寿命<=0 或飞出边界时用「尾部覆盖 + count--」删除。
 */
/*
 * 更新所有粒子：移动、重力曲线、递减寿命、火星二次爆裂、紧凑删除。
 * 参数：cols/rows - 画布尺寸。
 * 重力随存活时间增强：刚炸开时轻、临近熄灭时重，形成“先撑开再塌落”的节奏。
 */
static void update_particles(int cols, int rows)
{
    int i = 0;
    while (i < g_particle_count) {
        Particle *q = &g_particles[i];
        float age;
        q->vx  *= 0.988f;   /* 空气阻力：爆炸初期快、末段飘 */
        q->vy  *= 0.988f;
        q->x   += q->vx;
        q->y   += q->vy;
        age = 1.0f - (float)q->life / (float)(q->max_life > 0 ? q->max_life : 1);
        if (age < 0.0f) age = 0.0f;
        q->vy  += GRAVITY * (0.55f + 1.15f * age);
        if (q->dim >= 2) q->vy += GRAVITY * (WILLOW_GRAVITY_SCALE - 1.0f);
        q->life--;
        if (q->life <= 0 ||
            q->x < 0.0f || q->x >= (float)cols ||
            q->y < 0.0f || q->y >= (float)rows) {
            if (q->life <= 0 && q->kind == KIND_SPARK) {
                mini_burst(q->x, q->y);
            }
            g_particles[i] = g_particles[g_particle_count - 1];
            g_particle_count--;
        } else {
            i++;
        }
    }
}

/* ===== 渲染 ===== */

/*
 * 渲染火箭到画布（上升阶段可见，用 '|' 表示）。
 * 参数：r - 火箭结构体（只读）。
 */
static void render_rocket(const Rocket *r, int cols, int rows)
{
    int px, py;
    if (!r->active) return;
    px = (int)(r->x + 0.5f);
    py = (int)(r->y + 0.5f);
    if (px < 0 || px >= cols || py < 0 || py >= rows) return;
    draw_pixel(px, py, 0, '|');
    if (py + 1 < rows) draw_pixel(px, py + 1, 2, 'o');
    if (py + 2 < rows) draw_pixel(px, py + 2, 4, ',');
}

/*
 * 渲染所有粒子到画布，按距爆炸中心距离映射亮度字符。
 * 参数：cols/rows - 画布尺寸（用于跳过越界粒子）。
 */
/*
 * 渲染所有粒子到画布，按距爆炸中心距离映射亮度字符。
 * 参数：cols/rows - 画布尺寸（用于跳过越界粒子）。
 */
static void render_particles(int cols, int rows)
{
    int i;
    for (i = 0; i < g_particle_count; i++) {
        Particle *q = &g_particles[i];
        int px = (int)(q->x + 0.5f);
        int py = (int)(q->y + 0.5f);
        float dx, dy;
        int b;
        if (px < 0 || px >= cols || py < 0 || py >= rows) continue;
        dx = q->x - q->cx;
        dy = q->y - q->cy;
        b  = compute_brightness(dx, dy);
        /* 余烬变暗：寿命进入最后 1/3 时亮度降一档 */
        if (q->max_life > 0 && q->life * 3 < q->max_life && b < NUM_BRIGHT - 1) {
            b++;
        }
        b += q->dim;   /* 柳条整体更暗，像垂下的光丝 */
        /* 闪烁：按寿命与横坐标错相位，间歇压暗一格 */
        if (q->kind == KIND_TWINKLE &&
            ((q->life + (int)(q->x * 3.0f)) & 3) == 0 && b < NUM_BRIGHT - 1) {
            b++;
        }
        if (b > NUM_BRIGHT - 1) b = NUM_BRIGHT - 1;
        /* 移动拖尾：沿速度反方向补一个更暗的字符，形成速度线 */
        if (q->vx > 0.12f || q->vx < -0.12f || q->vy > 0.12f || q->vy < -0.12f) {
            /* 拖尾：按速度方向的 8 邻域取整，保证任何速度下都有 1~2 格可见尾巴 */
            int sx = (q->vx > 0.12f) - (q->vx < -0.12f);
            int sy = (q->vy > 0.12f) - (q->vy < -0.12f);
            int tb = b + 1 > NUM_BRIGHT - 1 ? NUM_BRIGHT - 1 : b + 1;
            draw_pixel(px - sx, py - sy, tb, BRIGHT_CHARS[tb]);
            tb = b + 3 > NUM_BRIGHT - 1 ? NUM_BRIGHT - 1 : b + 3;
            draw_pixel(px - 2 * sx, py - 2 * sy, tb, BRIGHT_CHARS[tb]);
        }
        draw_pixel(px, py, b, BRIGHT_CHARS[b]);
    }
}

/*
 * 构建帧缓冲并一次性输出（双缓冲）。
 * 参数：cols/rows - 画布有效尺寸。
 * 先 cursor_home 归位，再 fputs 整帧，最后 fflush。
 */
static void render_frame(int cols, int rows)
{
    int y;
    char *buf = g_framebuf;
    /* 防御：尺寸异常时宁可不画，也不能让 memcpy 越界 */
    if (cols <= 0 || rows <= 0 || cols > MAX_COLS || rows > MAX_ROWS) return;
    for (y = 0; y < rows; y++) {
        memcpy(buf, g_canvas[y], (size_t)cols);
        buf += cols;
        if (y < rows - 1) {
            *buf++ = '\n';
        }
    }
    *buf = '\0';
    cursor_home();
    fputs(g_framebuf, stdout);
    fflush(stdout);
}

/* ===== 用法与自检 ===== */

/*
 * 打印命令行用法。
 */
static void print_usage(const char *prog)
{
    printf("控制台字符烟花粒子系统 v1.0\n");
    printf("用法: %s [选项]\n\n", prog);
    printf("  --frames N     跑 N 帧后退出（默认 0 = 无限运行）\n");
    printf("  --fps F        目标帧率（默认 30）\n");
    printf("  --seed S       随机种子（默认取当前时间；固定后可复现）\n");
    printf("  --size WxH     强制画布尺寸，用于录屏/测试（如 120x36）\n");
    printf("  --ansi         每帧用 ANSI 光标归位，便于重定向抓帧\n");
    printf("  --selftest     运行内置自检后退出（返回码 0 表示全部通过）\n");
    printf("  --help, -h     显示本帮助\n\n");
    printf("运行中按任意键退出。\n");
}

/*
 * 内置自检：覆盖亮度映射、边界判界、粒子池上限与紧凑删除。
 * 返回：0 全部通过，非 0 为失败项数。
 */
static int run_selftest(void)
{
    int fail = 0;
    int saved_count = g_particle_count;
    long saved_drop = g_drop_count;
    int i;

    /* 1. 亮度映射：中心最亮、远处最暗、单调不增 */
    if (compute_brightness(0.0f, 0.0f) != 0) { printf("[FAIL] 中心亮度不是 @\n"); fail++; }
    if (compute_brightness(50.0f, 50.0f) != NUM_BRIGHT - 1) { printf("[FAIL] 远处亮度不是 .\n"); fail++; }
    for (i = 0; i < NUM_BRIGHT; i++) {
        float d = (float)i * 1.2f;
        if (compute_brightness(d, 0.0f) > compute_brightness(d - 0.1f, 0.0f)) {
            printf("[FAIL] 亮度随距离不单调\n");
            fail++;
            break;
        }
    }

    /* 2. 越界绘制：不应崩溃、不应写入画布外 */
    draw_pixel(-1, 0, 0, '@');
    draw_pixel(0, -1, 0, '@');
    draw_pixel(MAX_COLS, 0, 0, '@');
    draw_pixel(0, MAX_ROWS, 0, '@');
    if (g_canvas[0][0] != ' ' && g_canvas[0][0] != '|') { /* 允许已有内容 */ }

    /* 3. 粒子池上限：塞满后必须拒绝并计数 */
    g_particle_count = 0;
    g_drop_count = 0;
    for (i = 0; i < MAX_PARTICLES; i++) {
        (void)spawn_particle(1.0f, 1.0f, 1.0f, 1.0f, 0.1f, 0.1f, 10, KIND_NORMAL, 0);
    }
    if (g_particle_count != MAX_PARTICLES) { printf("[FAIL] 粒子池未灌满\n"); fail++; }
    if (spawn_particle(1.0f, 1.0f, 1.0f, 1.0f, 0.1f, 0.1f, 10, KIND_NORMAL, 0) != 0) {
        printf("[FAIL] 满池仍接受粒子\n");
        fail++;
    }
    if (g_drop_count != 1) { printf("[FAIL] 丢弃计数不对\n"); fail++; }

    /* 4. 紧凑删除：3 个粒子同时到期后 count 必须归零 */
    g_particle_count = 0;
    for (i = 0; i < 3; i++) {
        (void)spawn_particle(5.0f, 5.0f, 5.0f, 5.0f, 0.0f, 0.0f, 1, KIND_NORMAL, 0);
    }
    update_particles(80, 24);
    if (g_particle_count != 0) { printf("[FAIL] 到期粒子未清空\n"); fail++; }

    g_particle_count = saved_count;
    g_drop_count = saved_drop;

    if (fail == 0) {
        printf("SELFTEST PASS（亮度映射 / 边界判界 / 粒子池上限 / 紧凑删除）\n");
    } else {
        printf("SELFTEST FAIL：%d 项\n", fail);
    }
    return fail;
}

/* ===== 主循环 ===== */

/*
 * 入口：解析参数 -> 初始化 -> 主循环（更新->渲染->休眠）-> 清理退出。
 */
int main(int argc, char **argv)
{
    int frames, fps, frame_interval;
    unsigned seed;
     int cols = 0, rows = 0;   /* 必须先初始化：get_console_size 会读旧值做有效性判断 */
     int frame_count = 0;
     int i;

     parse_args(argc, argv, &frames, &fps, &seed, &g_ansi_mode);
     if (g_help) {
         print_usage(argv[0]);
         return 0;
     }
     if (g_selftest) {
         return run_selftest();
     }
     if (seed == 0) seed = (unsigned)time(NULL);
     srand(seed);

    get_console_size(&cols, &rows);
    /* --size 覆盖：录屏/自动化测试时不受实际窗口限制（仍受 MAX 上限保护） */
    if (g_force_cols >= 10 && g_force_rows >= 5) {
        cols = g_force_cols > MAX_COLS ? MAX_COLS : g_force_cols;
        rows = g_force_rows > MAX_ROWS ? MAX_ROWS : g_force_rows;
    }
     frame_interval = 1000 / fps;

     init_terminal();
     hide_cursor();
     clear_screen();

     /* 主循环：按键检测 -> 更新 -> 渲染 -> 帧同步 */
     while (frames == 0 || frame_count < frames) {
         double start_ms;

         /* --- 交互退出：任意键非阻塞检测 --- */
         if (check_key_pressed()) break;

         /* R4: periodic terminal size adaptation（--size 固定画布时跳过） */
         if (g_force_cols <= 0 && frame_count > 0 && frame_count % RESIZE_CHECK_FRAMES == 0) {
             int nc = cols, nr = rows;
             get_console_size(&nc, &nr);
             if (nc != cols || nr != rows) {
                 cols = nc;
                 rows = nr;
                 clear_screen();
             }
         }

         start_ms = get_time_ms();

         /* --- 更新阶段 --- */
         /* 多烟花发射调度：冷却到 0 时找空位发射并重置随机间隔 */
         if (g_launch_cooldown <= 0) {
             launch_rocket(cols, rows);
             g_launch_cooldown = LAUNCH_INTERVAL_MIN +
                 rand() % (LAUNCH_INTERVAL_MAX - LAUNCH_INTERVAL_MIN + 1);
         } else {
             g_launch_cooldown--;
         }
         for (i = 0; i < MAX_ROCKETS; i++) {
             update_rocket(&g_rockets[i], cols, rows);
         }
         update_particles(cols, rows);

         /* --- 渲染阶段 --- */
         clear_canvas();
         for (i = 0; i < MAX_ROCKETS; i++) {
             render_rocket(&g_rockets[i], cols, rows);
         }
         render_particles(cols, rows);
         render_frame(cols, rows);

         /* --- 帧同步：按实际耗时休眠剩余时间 --- */
         sync_frame(start_ms, frame_interval);
         frame_count++;
     }

     /* 清理退出：恢复终端与光标，保留最后一帧画面（不清屏，方便观察与截图） */
     restore_terminal();
     show_cursor();
     fputs("\n", stdout);
     fflush(stdout);
     return 0;
}
