

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "event_groups.h"
#include "xil_printf.h"

#define TASK_STK_SIZE       512
#define TASK_BASSE_PRIO     2
#define TASK_HAUTE_PRIO     3
#define All_Tasks           0x01

SemaphoreHandle_t Mutex;
SemaphoreHandle_t Sem;
EventGroupHandle_t FlagGroup;

int Temp;

void TaskBASSE(void *pvParameters)
{
    int x = 1, y = 2;

    while (1)
    {

        if (xSemaphoreTake(Mutex, portMAX_DELAY) == pdTRUE)
        {
            Temp = x;
            x = y;

            xil_printf("\nTB - Je suis dans la section critique et j'émule une interruption, qui a travers un service permet de debloquer Tache HAUTE\n");
            xSemaphoreGive(Sem);

            y = Temp;

            xil_printf("\nTB - Je m'apprête à sortir du mutex\n");
            xSemaphoreGive(Mutex);

            xil_printf("\nTB - %d %d\n", x, y);
        }

        vTaskDelay(pdMS_TO_TICKS(3000));    // Attente de 3 secondes et on recommence
    }
}

void TaskHAUTE(void *pvParameters)
{
    int x = 3, y = 4;

    while (1)
    {

        xil_printf("\nTH - je pars et j'attends \n");
        xSemaphoreTake(Sem, portMAX_DELAY);
        xil_printf("\nTH - je viens de recevoir le service d'une interruption\n");

        if (xSemaphoreTake(Mutex, portMAX_DELAY) == pdTRUE)
        {
            xil_printf("\nTH - Je rentre dans la section critique\n");
            Temp = x;
            x = y;
            y = Temp;

            xSemaphoreGive(Mutex);
        }

        xil_printf("\nTH - Je viens de sortir du mutex\n");
        xil_printf("\nTH - %d %d\n", x, y);

        vTaskDelay(pdMS_TO_TICKS(3000));    // Attente de 3 secondes et on recommence
    }
}

int main(void)
{
    xil_printf("START\n");
    Mutex = xSemaphoreCreateMutex();
    Sem = xSemaphoreCreateBinary();

    xTaskCreate(TaskBASSE, "TaskBASSE", TASK_STK_SIZE, NULL, TASK_BASSE_PRIO, NULL);
    xTaskCreate(TaskHAUTE, "TaskHAUTE", TASK_STK_SIZE, NULL, TASK_HAUTE_PRIO, NULL);

    vTaskStartScheduler();

    xil_printf("ERROR: Scheduler failed to start\r\n");

    for (;;);
    return 0;
}