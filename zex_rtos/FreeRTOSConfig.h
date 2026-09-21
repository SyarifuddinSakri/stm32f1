#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCPU_CLOCK_HZ ((unsigned long)72000000)
#define configSYSTICK_CLOCK_HZ (configCPU_CLOCK_HZ / 8)
#define configTICK_RATE_HZ ((TickType_t)250)
