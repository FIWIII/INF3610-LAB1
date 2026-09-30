/*
*********************************************************************************************************
*                                                 FreeRTOS
*
*
*            		          					            Guy Bois, Loic Nguemegne
*                                  Polytechnique Montreal, Qc, CANADA
*                                                  09/2026
*
*
*********************************************************************************************************
*/

#include "routeur.h"

#include <inttypes.h>
#include <projdefs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <xparameters.h>

#include <xil_printf.h>
#include "xil_io.h"

#include <stdio.h>

#include <xgpio.h>
#include <xil_exception.h>
#include <xintc.h>

#define LLONG_MAX 9223372036854775807

// Registres du compteur materiel Global Timer du Cortex-A9.
#define GLOBAL_TIMER_COUNTER_L  0xF8F00200U
#define GLOBAL_TIMER_COUNTER_H  0xF8F00204U
#define GLOBAL_TIMER_CONTROL    0xF8F00208U

// Frequence utilisee dans l'exemple timestamp.c fourni.
#define GLOBAL_TIMER_FREQ_HZ (XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2U)
    
uint64_t freq_hz;

// Memorise le compteur au debut pour calculer la duree totale.
static uint64_t debut_execution_gt;

uint64_t max_delay_video = 0L;
uint64_t max_delay_audio = 0L;
uint64_t max_delay_autre = 0L;

uint64_t average_blocking_mutex = 0L;
uint64_t average_blocking_sem = 0L;


UBaseType_t TaskQueueingQMax = 0;
UBaseType_t TaskComputing_HighQMax = 0;
UBaseType_t TaskComputing_MediumQMax = 0;
UBaseType_t TaskComputing_LowQMax = 0;


// � utiliser pour suivre le remplissage et le vidage des fifos
// Mettre en commentaire et utiliser la fonction vide suivante si vous ne voulez
// pas de trace

#if FULL_TRACE == 1

#define safeprintf(fmt, ...)                                                   \
  do {                                                                         \
    if (mutPrint != NULL) {                                                    \
      if (xSemaphoreTake(mutPrint, portMAX_DELAY) == pdTRUE) {                 \
        xil_printf(fmt, ##__VA_ARGS__);                                        \
        xSemaphoreGive(mutPrint);                                              \
      }                                                                        \
    }                                                                          \
  } while (0)
#else
#define safeprintf(fmt, ...)                                                   \
  do {                                                                         \
  } while (0)
#endif

/*
*********************************************************************************************************
*                                                  MAIN
*********************************************************************************************************
*/

int main(void) {

  create_application();

  vTaskStartScheduler();

  while (1)
    ;
  return 0; // Start multitasking
}

void create_application() {
  int error;

  error = create_events();
  if (error != 0)
    xil_printf("Error %d while creating events\n", error);

  error = create_tasks();
  if (error != 0)
    xil_printf("Error %d while creating tasks\n", error);
}

int taskCreationErrorCheck(TaskHandle_t handle) {
  if (handle == NULL) {
    return -1;
  }

  return 0;
}

int create_tasks() {

  StartupTaskHandler = xTaskCreateStatic(
      StartupTask,         // Fonction de la tâche
      "StartUp Task",      // Nom (à des fins de debug)
      TASK_STK_SIZE,       // Taille de la pile (en mots, pas octets)
      NULL,                // Paramètre (équivalent à (void*)0)
      MaxTaskPrio,         // Priorité
      &StartupTaskStk[0u], // Pointeur vers la pile
      &StartupTaskTCB      // Pointeur vers le TCB statique
  );

  return taskCreationErrorCheck(StartupTaskHandler);
}

int create_events() {
  // Creation des semaphores
  Sem_MemBlock = xSemaphoreCreateCounting(10000, // Valeur maximale
                                          10000  // Valeur initiale
  );
  Sem = xSemaphoreCreateCounting(1, 0);
  vSemaphoreCreateBinary(SemTaskComputing);

  // Creation des mutex
  mutPrint = xSemaphoreCreateMutex();
  mutAlloc = xSemaphoreCreateMutex();
  mutTaskComputing = xSemaphoreCreateMutex();

  // Creation des files externes - va servir � la manipulation 2

  source_errQ = xQueueCreate(1024, sizeof(Packet *));
  crc_errQ = xQueueCreate(1024, sizeof(Packet *));
  TaskQueueingQ = xQueueCreate(1024, sizeof(Packet *));
  TaskStatsQ = xQueueCreate(1024, sizeof(Packet *));
  for (int i = 0; i < NB_FIFO; i++) {
    TaskComputingQ[i] = xQueueCreate(1024, sizeof(Packet *));
    TaskOutputPortQ[i] = xQueueCreate(1024, sizeof(Packet *));
  }

  RouterStatus = xEventGroupCreate();

  return 0;
}

static uint64_t GlobalTimer_Read64(void)
{
    uint32_t hi1, lo, hi2;

    // Le compteur 64 bits est accessible par deux registres de 32 bits.
    // On relit la partie haute pour detecter un debordement de la
    // partie basse pendant la lecture.
    do {
        hi1 = Xil_In32(GLOBAL_TIMER_COUNTER_H);
        lo  = Xil_In32(GLOBAL_TIMER_COUNTER_L);
        hi2 = Xil_In32(GLOBAL_TIMER_COUNTER_H);
    } while (hi1 != hi2);

    // Conversion en 64 bits AVANT le decalage.
    // La partie haute occupe les bits 63 a 32, la basse les bits 31 a 0.
    return (((uint64_t)hi1) << 32) | lo;
}

static void GlobalTimer_Start(void)
{
    // Active le compteur en positionnant le bit 0 du registre
    // de controle, tout en conservant les autres bits.
    uint32_t ctrl = Xil_In32(GLOBAL_TIMER_CONTROL);
    ctrl |= 1U;
    Xil_Out32(GLOBAL_TIMER_CONTROL, ctrl);
}

void Update_TS(Packet *packet) {

  uint64_t delay;

  delay = xTaskGetTickCount() -
          packet->timestamp; // Valeur courante - valeur initiale

  if (delay < 0) {
    xil_printf("Attention overflow\n");
  }

  else {

    switch (packet->type) {
    case PACKET_VIDEO:
      if (delay > max_delay_video) {
        max_delay_video = delay;
      }
      break;

    case PACKET_AUDIO:
      if (delay > max_delay_audio) {
        max_delay_audio = delay;
      }
      break;

    case PACKET_AUTRE:
      if (delay > max_delay_autre) {
        max_delay_autre = delay;
      }
      break;

    default:
      break;
    }
  }
}

void UpdateQueueMax(QueueHandle_t fifo, UBaseType_t *fifoMax)
{
    UBaseType_t n = uxQueueMessagesWaiting(fifo);

    if (n > *fifoMax)
        *fifoMax = n;
}

/*
*********************************************************************************************************
*                                               STARTUP TASK
*********************************************************************************************************
*/

///////////////////////////////////////////////////////////////////////////////////////
//									TASKS
///////////////////////////////////////////////////////////////////////////////////////

/*
 *********************************************************************************************************
 *											  TaskGeneratePacket
 *  - G�n�re des paquets et les envoie dans la InputQ.
 *
 *
 *********************************************************************************************************
 */

void TaskGenerate(void *data) {
  srand(42);
  uint64_t ts;
  bool isGenPhase =
      false; // Indique si on est dans la phase de generation ou non
  int nb_rafales = 0;
  int packGenQty = (rand() % 255);
  while (true) {
    xEventGroupWaitBits(
        RouterStatus,      // Event group handle
        TASK_GENERATE_RDY, // Bits à attendre
        pdFALSE,      // Efface les bits après détection (comportement µC/OS)
        pdTRUE,       // Attendre que TOUS les bits soient SET
        portMAX_DELAY // Blocage infini (équivalent timeout = 0 µC/OS)
    );
    if (isGenPhase) {
      // Nouveau paquet

      Packet *packet = (Packet *)pvPortMalloc(sizeof(Packet));

      if (packet == NULL) {
        xil_printf("\nTaskGenerate: attention packet no %d est un NULL Pointer",
                   nbPacketCrees);
      };

      packet->src = rand() * (UINT32_MAX / RAND_MAX);
      packet->dst = rand() * (UINT32_MAX / RAND_MAX);
      packet->type = rand() % NB_PACKET_TYPE;

      for (int i = 0; i < ARRAY_SIZE(packet->data); ++i)
        packet->data[i] = (unsigned int)rand();

#if PERFORMANCE_TRACE == 1
      packet->timestamp = xTaskGetTickCount();
#endif

      packet->data[0] = ++nbPacketCrees;

      // Calcul du CRC avec injection de fautes
      packet->crc = 0;
      packet->crc = computeCRC((uint16_t *)(packet), sizeof(*packet));
      if (nbPacketCrees % 100 == 0)
        packet->crc++;

#if FULL_TRACE == 1
      xSemaphoreTake(mutPrint, portMAX_DELAY);
      xil_printf(
          "\nTaskGenerate : ********Generation du Paquet # %d ******** \n",
          nbPacketCrees);
      xil_printf("ADD %x \n", packet);
      xil_printf("	** id : %d \n", packet->data[0]);
      xil_printf("	** src : %x \n", packet->src);
      xil_printf("	** dst : %x \n", packet->dst);
      xil_printf("	** type : %d \n", packet->type);
      xSemaphoreGive(mutPrint);
#endif

      BaseType_t result;

      result = xQueueSendToBack(TaskQueueingQ, &packet,
                                0 // Timeout 0 : pas d'attente si queue pleine
      );
      

      if (result != pdPASS) {

        safeprintf("\nTaskGenerate: Paquet rejete TaskQueuingQ !\n");
        

#if FULL_TRACE == 1
        xQueueSendToBack(TaskStatsQ, &packet, 0);

#else
        vPortFree((void *)packet);
#endif
        nbPacketFIFOpleine++;
      }

      else {
          UpdateQueueMax(TaskQueueingQ, &TaskQueueingQMax);
          safeprintf(
            "\nTaskGenenerate: nb de paquets dans TaskQueueingQ - "
            "apres production: %d \n",
            uxQueueMessagesWaiting(TaskQueueingQ));
      }

      if ((nbPacketCrees % packGenQty) ==
          0) // On gen�re au maximum 255 paquets par phase de g�neration
      {
        safeprintf("\n***** TaskGenerate: FIN DE LA RAFALE No %d \n\n",
                   nb_rafales);
        isGenPhase = false;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(delai_pour_vider_les_fifos_msec));
      isGenPhase = true;
      do {
        packGenQty = (rand() % 255);
      } while (packGenQty == 0);

      safeprintf("\n***** TaskeGenerate: RAFALE No %d DE %d PAQUETS DURANT LES "
                 "%d PROCHAINES MILLISECONDES\n\n",
                 ++nb_rafales, packGenQty, packGenQty);
      safeprintf("\n***** TaskGenerate: DEMARRAGE \n\n");
    }
  }
}

/*
 *********************************************************************************************************
 *											  TaskReset
 *
 *********************************************************************************************************
 */
void TaskReset(void *data) {
  while (true) {
    xil_printf("--------------------- Task Reset --------------------\n");
    xEventGroupSetBits(RouterStatus, TASKS_ROUTER);
    EventBits_t flags = xEventGroupGetBits(RouterStatus);
    xil_printf("--------------------- Flags: %x "
               "---------------------------------------\n",
               flags);
    vTaskSuspend(TaskResetHandler);
  }
}

void TaskStop(void *data) {

  uint64_t nb_ticks;
  uint64_t sec;
  uint64_t nsec;

  // Variables pour mesurer la duree avec le compteur materiel.
  uint64_t fin_execution_gt;
  uint64_t duree_gt;
  uint64_t sec_gt;
  uint64_t nsec_gt;

  xSemaphoreTake(Sem, portMAX_DELAY);
  // La fin est mesuree avant les affichages de TaskStop.
  fin_execution_gt = GlobalTimer_Read64();

  // Suspend all tasks except statistics one
  xil_printf(
      "--------------------- Task stop suspend all tasks -------------\n");
      
  nb_ticks = (uint64_t)xTaskGetTickCount();
  sec  = nb_ticks / configTICK_RATE_HZ;
  nsec = ((nb_ticks % configTICK_RATE_HZ) * 1000000000ULL)
        / configTICK_RATE_HZ;
  xil_printf("Temps total d'execution : '%d.%09d' s\r\n",(int)sec, (int)nsec);

  // Nombre d'increments du compteur entre le debut et la fin.
  duree_gt = fin_execution_gt - debut_execution_gt;

  // Quotient : nombre de secondes completes.
  sec_gt = duree_gt / GLOBAL_TIMER_FREQ_HZ;

  // Reste : fraction de seconde convertie en nanosecondes.
  nsec_gt = ((duree_gt % GLOBAL_TIMER_FREQ_HZ) * 1000000000ULL)
            / GLOBAL_TIMER_FREQ_HZ;

  xil_printf("Temps total d'execution Global Timer : '%u.%09u' s\r\n", (unsigned int)sec_gt, (unsigned int)nsec_gt);
           
  xEventGroupClearBits(RouterStatus, TASKS_ROUTER);
  EventBits_t flags = xEventGroupGetBits(RouterStatus);
  xil_printf("--------------------- Flags: %x "
             "---------------------------------------\n",
             flags);
  vTaskSuspend(TaskStopHandler);
}



/*
 *********************************************************************************************************
 *                                            computeCRC
 * -Calcule la check value d'un pointeur quelconque (cyclic redudancy check)
 * -Retourne 0 si le CRC est correct, une autre valeur sinon.
 *********************************************************************************************************
 */

unsigned int computeCRC(uint16_t *w, int nleft) {

  unsigned int sum = 0;
  unsigned int nb_calls = 0;
  unsigned int Nb_of_ticks_in_CRC_init = 0;

  uint16_t answer = 0;

  // Code � compl�ter pour le calcul du nombre de ticks dans la manipulation 1

  // Adding words of 16 bits
  while (nleft > 1) {
    sum += *w++;
    nleft -= 2;
  }

  // Handling the last byte
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  // Handling overflow
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);

  answer = ~sum;

  // Code � compl�ter pour le calcul du nombre de ticks dans la manipulation 1

  return answer;
}

/*
 *********************************************************************************************************
 *											  TaskQueeing
 *
 *********************************************************************************************************
 */
void TaskQueueing(void *pdata) {
  uint64_t ts;
  BaseType_t result;
  Packet *packet = NULL;
  uint64_t actualticks = 0;
  while (true) {
    xEventGroupWaitBits(RouterStatus, TASK_QUEUING_RDY, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    xQueueReceive(TaskQueueingQ, &packet, portMAX_DELAY);
    safeprintf("\nTaskQueueing: nb de paquets apres consommation du fifo: %d \n",
               uxQueueMessagesWaiting(TaskQueueingQ));

    if (computeCRC((uint16_t *)packet, sizeof(*packet)) != 0) {

      safeprintf("\nTaskQueueing: Mauvais CRC !\n");

#if FULL_TRACE == 1
      xQueueSendToBack(TaskStatsQ, &packet, 0);
#else
      vPortFree((void *)packet);
#endif
      ++nbPacketMauvaisCRC;
    }

    else {

      // Dispatche les paquets selon leur type
      switch (packet->type) {
      case PACKET_VIDEO:
        result = xQueueSendToBack(TaskComputingQ[PACKET_VIDEO], &packet, 0);
        if (result == pdPASS) {
            safeprintf("\nTaskQueueing: nb de paquets dans TaskComputingQ[0] - HIGHQ "
                        "apres production : %d \n",
                        uxQueueMessagesWaiting(TaskComputingQ[PACKET_VIDEO]));
            UpdateQueueMax(TaskComputingQ[PACKET_VIDEO], &TaskComputing_HighQMax);
            }            
        break;

      case PACKET_AUDIO:
        result = xQueueSendToBack(TaskComputingQ[PACKET_AUDIO], &packet, 0);
        if (result == pdPASS) {
            safeprintf("\nTaskQueueing: nb de paquets dans TaskComputingQ[1] - MEDIUMQ "
                        "apres production : %d \n",
                        uxQueueMessagesWaiting(TaskComputingQ[PACKET_AUDIO]));
            UpdateQueueMax(TaskComputingQ[PACKET_AUDIO], &TaskComputing_MediumQMax);
            }  
        break;

      case PACKET_AUTRE:
        result = xQueueSendToBack(TaskComputingQ[PACKET_AUTRE], &packet, 0);
        if (result == pdPASS) {
        safeprintf("\nTaskQueueing: nb de paquets dans TaskComputingQ[2] - LOWQ "
                   "apres production: %d \n",
                   uxQueueMessagesWaiting(TaskComputingQ[PACKET_AUTRE]));
        UpdateQueueMax(TaskComputingQ[PACKET_AUTRE], &TaskComputing_LowQMax);
            }  
        break;

      default:
        safeprintf("\nTaskQueueing: Erreur sur la priorite du paquet - %d \n",
                   packet->data[0]);
#if FULL_TRACE == 1
        xQueueSendToBack(TaskStatsQ, &packet, 0);
#else
        vPortFree((void *)packet);
#endif
        nbPacketMauvaisePriorite++;
        break;
      }

      if (result != pdPASS) {
        safeprintf(
            "\nTaskQueueing: Paquet rejete de FIFO no %d de TaskComputing\n",
            packet->type);
#if FULL_TRACE == 1
        xQueueSendToBack(TaskStatsQ, &packet, 0);
#else
        vPortFree((void *)packet);
#endif
        nbPacketFIFOpleine++;
      }
    }
  }
}

/*
 *********************************************************************************************************
 *											  TaskComputing
 *  -V�rifie si les paquets sont conformes i.e. qu on emule un CRC et on verifie
 *l espace addresse -Dispatche les paquets dans des files (HIGH,MEDIUM,LOW)
 *
 *********************************************************************************************************
 */
void TaskComputing(void *pdata) {
  uint64_t ts;
  uint64_t t1, t2;
  BaseType_t result;
  Packet *packet = NULL;
  uint64_t actualticks = 0;
  Info_FIFO info = *(Info_FIFO *)pdata;
  int WAITFORTICKS;
  float nombre;
  // Variables pour l'attente active mesuree avec le Global Timer.
  uint32_t delay_ms;
  uint64_t delay_counts;
  uint64_t start_counts;

  while (true) {
    xEventGroupWaitBits(RouterStatus, TASK_COMPUTING_RDY, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    xQueueReceive(TaskComputingQ[info.id], &packet, portMAX_DELAY);

    safeprintf(
        "\nTaskComputing %s: nb de paquets apres consommation du fifo: %d \n",
        info.name, uxQueueMessagesWaiting(TaskComputingQ[info.id]));

    // Verification de l'espace d'addressage
    if ((packet->src > REJECT_LOW1 && packet->src < REJECT_HIGH1) ||
        (packet->src > REJECT_LOW2 && packet->src < REJECT_HIGH2) ||
        (packet->src > REJECT_LOW3 && packet->src < REJECT_HIGH3)) {

      safeprintf("\nTaskComputing %s: paquet mauvaise source\n", info.name);

#if FULL_TRACE == 1
      xQueueSendToBack(TaskStatsQ, &packet, 0);
#else
      vPortFree((void *)packet);

#endif
      nbPacketMauvaiseSource++;

    }

    else { // we can start processing and forwarding

      // we may emulate a certain time for the processing and also emulate
      // priority inheritance

      switch (packet->type) {

      case PACKET_VIDEO:
#if MUTEX == 1
        t1 = xTaskGetTickCount();
        xSemaphoreTake(mutTaskComputing, portMAX_DELAY);
        t2 = xTaskGetTickCount();
        average_blocking_mutex = average_blocking_mutex + (t2 - t1);
        ++nbPacketTraites_Video;
#elif SEMAPHORE == 1
        t1 = xTaskGetTickCount();
        xSemaphoreTake(SemTaskComputing, portMAX_DELAY) ;
        t2 = xTaskGetTickCount();
        average_blocking_sem = average_blocking_sem + (t2 - t1);
        ++nbPacketTraites_Video;
#endif

#if DELAI_0_1 == 1
        // Choisit une attente de 0 ou 1 milliseconde.
        delay_ms = rand() % 2;

        // Convertit les millisecondes en increments du Global Timer.
        delay_counts =
            ((uint64_t)GLOBAL_TIMER_FREQ_HZ / 1000ULL) * delay_ms;

        // Attend activement que la duree demandee soit ecoulee.
        start_counts = GlobalTimer_Read64();
        while ((GlobalTimer_Read64() - start_counts) < delay_counts) {
            // Attente active : la tache peut toujours etre preemptee.
            }
          ;
#elif DELAI_1_2 == 1
        do {
          WAITFORTICKS = (rand() % 3) * TICK_SCALING;
        } while (WAITFORTICKS == 0);
        actualticks = xTaskGetTickCount();
        while (WAITFORTICKS + actualticks > xTaskGetTickCount())
          ;
#endif

#if MUTEX == 1
        xSemaphoreGive(mutTaskComputing);
#elif SEMAPHORE == 1
        xSemaphoreGive(SemTaskComputing);
#endif
        break;

      case PACKET_AUDIO:

#if DELAI_0_1 == 1
         // Choisit une attente de 0 ou 1 milliseconde.
        delay_ms = rand() % 2;

        // Convertit les millisecondes en increments du Global Timer.
        delay_counts =
            ((uint64_t)GLOBAL_TIMER_FREQ_HZ / 1000ULL) * delay_ms;

        // Attend activement que la duree demandee soit ecoulee.
        start_counts = GlobalTimer_Read64();
        while ((GlobalTimer_Read64() - start_counts) < delay_counts) {
            // Attente active : la tache peut toujours etre preemptee.
            }
          ;
#elif DELAI_1_2 == 1
        do {
          WAITFORTICKS = (rand() % 3) * TICK_SCALING;
        } while (WAITFORTICKS == 0);
        actualticks = xTaskGetTickCount();
        while (WAITFORTICKS + actualticks > xTaskGetTickCount())
          ;
#endif
        break;

      case PACKET_AUTRE:
#if MUTEX == 1
        xSemaphoreTake(mutTaskComputing, portMAX_DELAY);
#elif SEMAPHORE == 1
        xSemaphoreTake(SemTaskComputing, portMAX_DELAY);
#endif

#if DELAI_0_1 == 1
         // Choisit une attente de 0 ou 1 milliseconde.
        delay_ms = rand() % 2;

        // Convertit les millisecondes en increments du Global Timer.
        delay_counts =
            ((uint64_t)GLOBAL_TIMER_FREQ_HZ / 1000ULL) * delay_ms;

        // Attend activement que la duree demandee soit ecoulee.
        start_counts = GlobalTimer_Read64();
        while ((GlobalTimer_Read64() - start_counts) < delay_counts) {
            // Attente active : la tache peut toujours etre preemptee.
            }
          ;
#elif DELAI_1_2 == 1
        do {
          WAITFORTICKS = (rand() % 3) * TICK_SCALING;
        } while (WAITFORTICKS == 0);
        actualticks = xTaskGetTickCount();
        while (WAITFORTICKS + actualticks > xTaskGetTickCount())
          ;
#endif

#if MUTEX == 1
        xSemaphoreGive(mutTaskComputing);
#elif SEMAPHORE == 1
        xSemaphoreGive(SemTaskComputing);
#endif
        break;

      default:
        xil_printf("\nATTENTION Paquet non identifi/ dans TaskComputing \n");
        break;
      };

      /* Test sur la destination du paquet */
      if (packet->dst >= INT1_LOW && packet->dst <= INT1_HIGH) {

        safeprintf("\nTaskComputing %s: paquet envoye dans Port 0 \n",
                   info.name);
#if PERFORMANCE_TRACE == 1
        Update_TS(packet);
#endif
        result = xQueueSendToBack(TaskOutputPortQ[PACKET_VIDEO], &packet, 0);
      } else {
        if (packet->dst >= INT2_LOW && packet->dst <= INT2_HIGH) {
          safeprintf("\nTaskComputing %s: paquet envoye dans Port 1 \n",
                     info.name);
#if PERFORMANCE_TRACE == 1
          Update_TS(packet);
#endif
          result = xQueueSendToBack(TaskOutputPortQ[PACKET_AUDIO], &packet, 0);
        } else {
          if (packet->dst >= INT3_LOW && packet->dst <= INT3_HIGH) {
            safeprintf("\nTaskComputing %s: paquet envoye dans Port 2 - \n",
                       info.name);
#if PERFORMANCE_TRACE == 1
            Update_TS(packet);
#endif
            result =
                xQueueSendToBack(TaskOutputPortQ[PACKET_AUTRE], &packet, 0);
          } else {
            if (packet->dst >= INT_BC_LOW && packet->dst <= INT_BC_HIGH) {
              Packet *others[2];
              int i;
              for (i = 0; i < ARRAY_SIZE(others); ++i) {
                others[i] = (Packet *)pvPortMalloc(sizeof(Packet));
                memcpy(others[i], packet, sizeof(Packet));
              }
              safeprintf("\nTaskComputing %s: paquet diffuser (BC) dans tous les "
                         "ports\n",
                         info.name);
#if PERFORMANCE_TRACE == 1
              Update_TS(packet);
              for (i = 0; i < ARRAY_SIZE(others); ++i) {
                Update_TS(others[i]);
              }
#endif
              result =
                  xQueueSendToBack(TaskOutputPortQ[PACKET_VIDEO], &packet, 0);
              result = xQueueSendToBack(TaskOutputPortQ[PACKET_AUDIO],
                                        &others[0], 0);
              result = xQueueSendToBack(TaskOutputPortQ[PACKET_AUTRE],
                                        &others[1], 0);
            }
          }
        }
      }

      if (result != pdPASS) {
        /*Destruction du paquet si la mailbox de destination est pleine*/

        safeprintf("\nTaskComputing %s: paquet rejete d'un des fifo de "
                   "TaskOutputPort!\n",
                   info.name);
#if FULL_TRACE == 1
        xQueueSendToBack(TaskStatsQ, &packet, 0);
#else
        vPortFree((void *)packet);
#endif
        nbPacketFIFOpleine++;
      } else {
        ++nbPacketTraites;
      }
    }
  }
}

/*
 *********************************************************************************************************
 *											  TaskPrint
 *  -Affiche les infos des paquets arriv�s � destination et libere la m�moire
 *allou�e
 *********************************************************************************************************
 */
void TaskOutputPort(void *data) {
  uint64_t ts;
  Packet *packet = NULL;
  Info_Port info = *(Info_Port *)data;

  while (1) {
    xEventGroupWaitBits(RouterStatus, TASK_OUTPUTPORT_RDY, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    /*Attente d'un paquet*/
    xQueueReceive(TaskOutputPortQ[info.id], &packet, portMAX_DELAY);

#if FULL_TRACE == 1
    xSemaphoreTake(mutPrint, portMAX_DELAY);
    xil_printf("\nTaskOutputPort: paquet recu sur port %s \n", Port[info.id].name); 
    xil_printf("	** id : %d \n", packet->data[0]);
    xil_printf("    >> src : %x \n", packet->src);
    xil_printf("    >> dst : %x \n", packet->dst);
    xil_printf("    >> type : %d \n", packet->type);
    xSemaphoreGive(mutPrint);
#endif
    /*Lib�ration de la m�moire*/
    vPortFree((void *)packet);
  }
}

/*
 *********************************************************************************************************
 *                                              TaskStats
 *  -Est d�clench�e lorsque le gpio_isr() lib�re le s�maphore
 *  -Lorsque d�clench�e, imprime les statistiques du routeur � cet instant
 *********************************************************************************************************
 */
void TaskStats(void *pdata) {
  uint64_t ts;
  Packet *packet = NULL;
  uint64_t total_freq;
  uint64_t sec;
  uint64_t nsec;

  while (1) {
    xEventGroupWaitBits(RouterStatus, TASK_STATS_RDY, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    xSemaphoreTake(mutPrint, portMAX_DELAY);

    xil_printf("\n------------------ Affichage des statistiques "
               "------------------\n\n");
    xil_printf("Delai pour vider les fifos msec: %d\n",
               delai_pour_vider_les_fifos_msec);
    xil_printf("Frequence du systeme: %d\n", configTICK_RATE_HZ);

#if MUTEX == 1
    xil_printf("Mode mutex ");
#elif SEMAPHORE == 1
    xil_printf("Mode semaphore ");
#else
    xil_printf("Pas de section critique ");
#endif

    xil_printf("\r\n");

#if DELAI_0_1 == 1
    xil_printf("DELAI_0_1");
#elif DELAI_1_2 == 1
    xil_printf("DELAI_1_2");
#else
    xil_printf("Pas d attente active");
#endif
    xil_printf("\r\n");

    xil_printf("1 - Nb de packets total crees : %d\n", nbPacketCrees);
    xil_printf("2 - Nb de packets total traites : %d\n", nbPacketTraites);

    nbPacketRejetes = nbPacketMauvaisCRC + nbPacketMauvaiseSource +
                      nbPacketFIFOpleine + nbPacketMauvaisePriorite;
    xil_printf("3 - Nb de packets rejetes pour mauvaise source : %d\n",
               nbPacketMauvaiseSource);
    xil_printf("4 - Nb de packets rejetes pour mauvaise source total: %d\n",
               nbPacketMauvaiseSourceTotal);
    xil_printf("5 - Nb de packets rejetes pour mauvais CRC : %d\n",
               nbPacketMauvaisCRC);
    xil_printf("6 - Nb de packets rejetes pour mauvais CRC total : %d\n",
               nbPacketMauvaisCRCTotal);
    xil_printf("7 - Nb de paquets rejetes fifo : %d\n", nbPacketFIFOpleine);
    xil_printf("8 - Nb de paquets rejetes fifo total : %d\n",
               nbPacketFIFOpleineTotal);
    xil_printf("9 - Nb de paquets rejetes mauvaise priorites : %d\n",
               nbPacketMauvaisePriorite);
    xil_printf("10 - Nb de paquets rejetes mauvaise priorites total : %d\n",
               nbPacketMauvaisePrioriteTotal);
    xil_printf("11 - Nb de paquets maximum dans le fifo de Queueing : %d \n",
               TaskQueueingQMax);
    xil_printf(
        "12 - Nb de paquets maximum dans le fifo HIGHQ de TaskComputing: %d \n",
        TaskComputing_HighQMax);
    xil_printf(
        "13 - Nb de paquets maximum dans fifo MEDIUMQ de TaskComputing: %d \n",
        TaskComputing_MediumQMax);
    xil_printf(
        "14 - Nb de paquets maximum dans fifo LOWQ de TaskComputing: %d \n",
        TaskComputing_LowQMax);
    xil_printf("15- Nombre de ticks depuis le d�but de l'execution %d \n",
               xTaskGetTickCount());

    xSemaphoreGive(mutPrint);
#if PERFORMANCE_TRACE == 1

sec  = max_delay_video / freq_hz;
nsec = ((max_delay_video % freq_hz) * 1000000000ULL) / freq_hz;

xil_printf("16- Pire temps video '%d.%09d' s\r\n",
           (int)sec, (int)nsec);

sec  = max_delay_audio / freq_hz;
nsec = ((max_delay_audio % freq_hz) * 1000000000ULL) / freq_hz;

xil_printf("17- Pire temps audio '%d.%09d' s\r\n",
           (int)sec, (int)nsec);
           
sec  = max_delay_autre / freq_hz;
nsec = ((max_delay_autre % freq_hz) * 1000000000ULL) / freq_hz;

xil_printf("18- Pire temps autre '%d.%09d' s\r\n",
           (int)sec, (int)nsec);

#if MUTEX == 1

total_freq = (uint64_t)nbPacketTraites_Video * freq_hz;

// Au premier affichage, aucun paquet video n'a encore ete traite.
// On evite donc une division par zero.
if (total_freq != 0) {
    sec = average_blocking_mutex / total_freq;
    nsec = ((average_blocking_mutex % total_freq) * 1000000000ULL)
           / total_freq;
} else {
    sec = 0;
    nsec = 0;
}

xil_printf("19- Attente de blocage moyen pour le mutex : "
           "'%d.%09d' s pour %d packets videos traites\r\n",
           (int)sec, (int)nsec,
           nbPacketTraites_Video);

#elif SEMAPHORE == 1

total_freq = (uint64_t)nbPacketTraites_Video * freq_hz;

sec  = average_blocking_mutex / total_freq;
nsec = ((average_blocking_mutex % total_freq) * 1000000000ULL)
       / total_freq;

xil_printf("19- Attente de blocage moyen pour le semaphore : "
           "'%d.%09d' s pour %d packets videos traites\r\n",
           (int)sec, (int)nsec,
           nbPacketTraites_Video);

#endif

#endif
    // On vide la fifo des paquets rejet�s et on imprime si l option est
    // demandee
#if FULL_TRACE == 1

    while (1) {
      xQueueReceive(TaskStatsQ, &packet, 0);

      if (packet == NULL) {
        break;
      } else {

        if (print_paquets_rejetes) {
          xSemaphoreTake(mutPrint, portMAX_DELAY);
          xil_printf("    >> paquet rejete # : %d \n", packet->data[0]);
          xil_printf("    >> src : %x \n", packet->src);
          xil_printf("    >> dst : %x \n", packet->dst);
          xil_printf("    >> type : %d \n", packet->type);
          xSemaphoreGive(mutPrint);
        }

        vPortFree((void *)packet);

        packet = NULL;
      };
    };

#endif

    nbPacketMauvaisCRCTotal += nbPacketMauvaisCRC;
    nbPacketMauvaisCRC = 0;

    nbPacketMauvaiseSourceTotal += nbPacketMauvaiseSource;
    nbPacketMauvaiseSource = 0;

    nbPacketFIFOpleineTotal += nbPacketFIFOpleine;
    nbPacketFIFOpleine = 0;

    nbPacketMauvaisePrioriteTotal += nbPacketMauvaisePriorite;
    nbPacketMauvaisePriorite = 0;

    nbPacketRejetesTotal =
        nbPacketMauvaisCRCTotal + nbPacketMauvaiseSourceTotal +
        nbPacketFIFOpleineTotal + nbPacketMauvaisePrioriteTotal;

    // On stoppe tout le programme quand on a atteint la limite de paquets
    if (nbPacketCrees > limite_de_paquets)
      xSemaphoreGive(Sem);

    // On imprime ls statistiques � toutes les 10 secondes
    TickType_t xDelay = pdMS_TO_TICKS(10000);
    vTaskDelay(xDelay);
  }
}

void err_msg(char *entete, uint8_t err) {
  if (err != 0) {
    xil_printf(entete);
    xil_printf(": Une erreur est retourn�e : code %d \n", err);
  }
}

void StartupTask(void *p_arg) {

   // Demarre le compteur et memorise l'instant de depart.
  GlobalTimer_Start();
  debut_execution_gt = GlobalTimer_Read64();
    
  // printf("UCOS - Total configured heap size. %d\r\n", seg_info.TotalSize);
  // printf("UCOS - Total used size after init. %d\r\n", seg_info.UsedSize);

  printf("Programme initialise\r\n");

  printf("Frequence courante du tick d horloge - %d\r\n", configTICK_RATE_HZ);

  // freq_hz = CPU_TS_TmrFreqGet(&err); /* Get CPU timestamp timer frequency. */
  freq_hz = configTICK_RATE_HZ;
  xil_printf("\nfreq du timestamp: %u\r\n", (unsigned int)freq_hz);
  
  // Affiche la frequence de l'horloge materielle utilisee pour l'attente.
  xil_printf("Global Timer freq = %u Hz\r\n",(unsigned int)GLOBAL_TIMER_FREQ_HZ);

  // On cr�e les t�ches

  for (int i = 0; i < NB_FIFO; i++) {
    switch (i) {
    case 0:
      FIFO[i].id = PACKET_VIDEO;
      FIFO[i].name = "HighQ";
      break;
    case 1:
      FIFO[i].id = PACKET_AUDIO;
      FIFO[i].name = "MediumQ";
      break;
    case 2:
      FIFO[i].id = PACKET_AUTRE;
      FIFO[i].name = "LowQ";
      break;
    default:
      break;
    };
  }

  for (int i = 0; i < NB_OUTPUT_PORTS; i++) {
    Port[i].id = i;
    switch (i) {
    case 0:
      Port[i].name = "Port 0";
      break;
    case 1:
      Port[i].name = "Port 1";
      break;
    case 2:
      Port[i].name = "Port 2";
      break;
    default:
      break;
    };
  }


if (MaxTaskPrio < TaskStatsPRIO) xil_printf(":Erreur priorité max trop haute\n");

  int err = 0;
  TaskgenerateHandler = xTaskCreateStatic(
      TaskGenerate,   // Fonction tâche
      "TaskGenerate", // Nom de la tâche (debug)
      TASK_STK_SIZE,  // Taille pile en mots (attention: pas TASK_STK_SIZE / 2)
      NULL,           // Paramètre passé à la tâche
      TaskGeneratePRIO, // Priorité (entre 0 et configMAX_PRIORITIES-1)
      TaskGenerateSTK,  // Pointeur pile (tableau)
      &TaskGenerateTCB  // Pointeur TCB statique
  );
  err |= taskCreationErrorCheck(TaskgenerateHandler);

  TaskQueueingHandler =
      xTaskCreateStatic(TaskQueueing, "TaskQueueing", TASK_STK_SIZE, NULL,
                        TaskQueueingPRIO, TaskQueueingSTK, &TaskQueueingTCB);
  err |= taskCreationErrorCheck(TaskQueueingHandler);

  for (int i = 0; i < NB_FIFO; i++) {
    TaskComputingHandler[i] = xTaskCreateStatic(
        TaskComputing, "TaskComputing", TASK_STK_SIZE, &FIFO[i],
        TaskComputingPRIO - i, TaskComputingSTK[i], &TaskComputingTCB[i]);
    err |= taskCreationErrorCheck(TaskComputingHandler[i]);
  }

  for (int i = 0; i < NB_OUTPUT_PORTS; i++) {
    TaskOutputPortHandler[i] = xTaskCreateStatic(
        TaskOutputPort, "OutputPort", TASK_STK_SIZE, &Port[i],
        TaskOutputPortPRIO, TaskOutputPortSTK[i], &TaskOutputPortTCB[i]);
    err |= taskCreationErrorCheck(TaskOutputPortHandler[i]);
  }

  TaskStatsHandler =
      xTaskCreateStatic(TaskStats, "TaskStats", TASK_STK_SIZE, NULL,
                        TaskStatsPRIO, TaskStatsSTK, &TaskStatsTCB);
  err |= taskCreationErrorCheck(TaskStatsHandler);

  TaskResetHandler =
      xTaskCreateStatic(TaskReset, "TaskReset", TASK_STK_SIZE, NULL,
                        TaskResetPRIO, TaskResetSTK, &TaskResetTCB);
  err |= taskCreationErrorCheck(TaskResetHandler);

  TaskStopHandler = xTaskCreateStatic(TaskStop, "TaskStop", TASK_STK_SIZE, NULL,
                                      TaskStopPRIO, TaskStopSTK, &TaskStopTCB);
  err |= taskCreationErrorCheck(TaskStopHandler);

  vTaskSuspend(NULL);
}