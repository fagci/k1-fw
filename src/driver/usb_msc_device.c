#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_bus.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_gpio.h"
#include "../external/printf/printf.h"
#include "bk4819-regs.h"
#include "bk4829.h"
#include "fat_fs.h"
#include "usbd_core.h"
#include "usbd_msc.h"

// Размер блока и количество блоков (2MB / 512 байт)
#define FLASH_BLOCK_COUNT 4096

/*!< endpoint address */
#define MSC_IN_EP 0x81
#define MSC_OUT_EP 0x01

// Используем стандартные VID/PID для тестирования
#define USBD_VID 0x0483 // STMicroelectronics
#define USBD_PID 0x5720 // Mass Storage
#define USBD_MAX_POWER 100
#define USBD_LANGID_STRING 1033

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define MSC_MAX_MPS 512
#else
#define MSC_MAX_MPS 64
#endif

/*!< global descriptor */
static const uint8_t msc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID,
                               0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, 0x00),
    ///////////////////////////////////////
    /// string0 descriptor
    ///////////////////////////////////////
    USB_LANGID_INIT(USBD_LANGID_STRING),
    ///////////////////////////////////////
    /// string1 descriptor
    ///////////////////////////////////////
    0x0A,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'P', 0x00,                  /* wcChar0 */
    'U', 0x00,                  /* wcChar1 */
    'Y', 0x00,                  /* wcChar2 */
    'A', 0x00,                  /* wcChar3 */
    ///////////////////////////////////////
    /// string2 descriptor
    ///////////////////////////////////////
    0x1A,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'P', 0x00,                  /* wcChar0 */
    'Y', 0x00,                  /* wcChar1 */
    '3', 0x00,                  /* wcChar2 */
    '2', 0x00,                  /* wcChar3 */
    ' ', 0x00,                  /* wcChar4 */
    'F', 0x00,                  /* wcChar5 */
    'l', 0x00,                  /* wcChar6 */
    'a', 0x00,                  /* wcChar7 */
    's', 0x00,                  /* wcChar8 */
    'h', 0x00,                  /* wcChar9 */
    ' ', 0x00,                  /* wcChar10 */
    'D', 0x00,                  /* wcChar11 */
    'i', 0x00,                  /* wcChar12 */
    's', 0x00,                  /* wcChar13 */
    'k', 0x00,                  /* wcChar14 */
    ///////////////////////////////////////
    /// string3 descriptor
    ///////////////////////////////////////
    0x16,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    '2', 0x00,                  /* wcChar0 */
    '0', 0x00,                  /* wcChar1 */
    '2', 0x00,                  /* wcChar2 */
    '4', 0x00,                  /* wcChar3 */
    '1', 0x00,                  /* wcChar4 */
    '2', 0x00,                  /* wcChar5 */
    '3', 0x00,                  /* wcChar6 */
    '4', 0x00,                  /* wcChar7 */
    '5', 0x00,                  /* wcChar8 */
    '6', 0x00,                  /* wcChar9 */
#ifdef CONFIG_USB_HS
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x40, 0x01, 0x00,
#endif
    0x00};

struct usbd_interface intf0;

// Колбэк вызывается после успешной конфигурации USB устройства
void usbd_configure_done_callback(void) {
  // Добавьте индикацию (LED мигает = устройство подключено)
  // GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
  BK4819_ToggleGpioOut(BK4819_GREEN, true);
}

// Опциональные колбэки (могут понадобиться в некоторых версиях CherryUSB)
__attribute__((weak)) void usbd_event_handler(uint8_t event) {
  // Отладка: можно вывести event через UART
  printf("USB Event: %d\n", event);

  switch (event) {
  case 0: // USBD_EVENT_RESET
    // Устройство сброшено хостом
    break;
  case 1: // USBD_EVENT_CONNECTED
    // Устройство подключено
    break;
  case 2: // USBD_EVENT_DISCONNECTED
    // Устройство отключено
    break;
  }
}

__attribute__((weak)) bool usbd_msc_get_write_protection(uint8_t lun) {
  return false; // Нет защиты от записи
}

__attribute__((weak)) void usbd_msc_notify_ready(uint8_t lun) {
  // Уведомление о готовности устройства
}

// Колбэки для CherryUSB MSC - вызываются библиотекой при SCSI командах
// Версия CherryUSB с LUN, но без busid

void usbd_msc_get_cap(uint8_t lun, uint32_t *block_num, uint16_t *block_size) {
  *block_num = FLASH_BLOCK_COUNT - 1; // Последний доступный блок (count - 1)
  *block_size = 512;
}

int usbd_msc_sector_read(uint32_t sector, uint8_t *buffer, uint32_t length) {
  if (length == 0) {
    return 0;
  }

  // Проверка границ
  if (sector >= FLASH_BLOCK_COUNT) {
    return -1; // Ошибка: вне диапазона
  }

  uint32_t block_count = length / 512;

  // Проверка что не выходим за границы
  if (sector + block_count > FLASH_BLOCK_COUNT) {
    return -1;
  }

  for (uint32_t i = 0; i < block_count; i++) {
    FAT_ReadBlock(sector + i, buffer + i * 512);
  }

  return 0;
}

int usbd_msc_sector_write(uint32_t sector, uint8_t *buffer, uint32_t length) {
  if (length == 0) {
    return 0;
  }

  // Проверка границ
  if (sector >= FLASH_BLOCK_COUNT) {
    return -1; // Ошибка: вне диапазона
  }

  uint32_t block_count = length / 512;

  // Проверка что не выходим за границы
  if (sector + block_count > FLASH_BLOCK_COUNT) {
    return -1;
  }

  for (uint32_t i = 0; i < block_count; i++) {
    FAT_WriteBlock(sector + i, buffer + i * 512);
  }

  return 0;
}

// Инициализация MSC устройства
void msc_init(void) {
  // Включаем тактирование USB и GPIO (на всякий случай)
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USBD);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);

  // ВАЖНО: Настройка GPIO для USB с pull-up на D+
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = LL_GPIO_PIN_11 | LL_GPIO_PIN_12;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO; // Внешний резистор 1.5кОм
  GPIO_InitStruct.Alternate = LL_GPIO_AF_2; // USB AF
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // Инициализация FAT файловой системы
  FAT_Init();

  // Регистрация дескрипторов
  usbd_desc_register(msc_descriptor);

  // Инициализация MSC интерфейса
  usbd_add_interface(usbd_msc_init_intf(&intf0, MSC_OUT_EP, MSC_IN_EP));

  // Включаем прерывание USB
  NVIC_SetPriority(USBD_IRQn, 0);
  NVIC_EnableIRQ(USBD_IRQn);

  // Инициализация USB стека
  usbd_initialize();
}
