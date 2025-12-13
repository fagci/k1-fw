#include <stdint.h>

// USB Device Descriptor
const uint8_t USB_DeviceDescriptor[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0x00,        // bDeviceClass (определяется в интерфейсе)
    0x00,        // bDeviceSubClass
    0x00,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 64
    0x83, 0x04,  // idVendor 0x0483 (STMicroelectronics)
    0x40, 0x57,  // idProduct 0x5740
    0x00, 0x02,  // bcdDevice 2.00
    0x01,        // iManufacturer (String Index)
    0x02,        // iProduct (String Index)
    0x03,        // iSerialNumber (String Index)
    0x01         // bNumConfigurations 1
};

// USB Configuration Descriptor
const uint8_t USB_ConfigDescriptor[] = {
    // Configuration Descriptor
    0x09,        // bLength
    0x02,        // bDescriptorType (Configuration)
    0x20, 0x00,  // wTotalLength 32 байта
    0x01,        // bNumInterfaces 1
    0x01,        // bConfigurationValue
    0x00,        // iConfiguration (String Index)
    0x80,        // bmAttributes (Bus Powered)
    0xFA,        // bMaxPower 500mA
    
    // Interface Descriptor
    0x09,        // bLength
    0x04,        // bDescriptorType (Interface)
    0x00,        // bInterfaceNumber 0
    0x00,        // bAlternateSetting
    0x02,        // bNumEndpoints 2
    0x08,        // bInterfaceClass (Mass Storage)
    0x06,        // bInterfaceSubClass (SCSI)
    0x50,        // bInterfaceProtocol (Bulk-Only)
    0x00,        // iInterface (String Index)
    
    // Endpoint Descriptor (Bulk IN)
    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x81,        // bEndpointAddress (IN, Endpoint 1)
    0x02,        // bmAttributes (Bulk)
    0x40, 0x00,  // wMaxPacketSize 64
    0x00,        // bInterval 0
    
    // Endpoint Descriptor (Bulk OUT)
    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x01,        // bEndpointAddress (OUT, Endpoint 1)
    0x02,        // bmAttributes (Bulk)
    0x40, 0x00,  // wMaxPacketSize 64
    0x00         // bInterval 0
};

// String Descriptors
const uint8_t USB_StringLangID[] = {
    0x04, 0x03, 0x09, 0x04  // Language ID: 0x0409 (English US)
};

const uint8_t USB_StringManufacturer[] = {
    0x0E, 0x03,  // bLength, bDescriptorType
    'P', 0, 'Y', 0, '3', 0, '2', 0, 'F', 0, '0', 0
};

const uint8_t USB_StringProduct[] = {
    0x1A, 0x03,  // bLength, bDescriptorType
    'M', 0, 'a', 0, 's', 0, 's', 0, ' ', 0, 
    'S', 0, 't', 0, 'o', 0, 'r', 0, 'a', 0, 'g', 0, 'e', 0
};

const uint8_t USB_StringSerial[] = {
    0x1A, 0x03,  // bLength, bDescriptorType
    '1', 0, '2', 0, '3', 0, '4', 0, '5', 0, 
    '6', 0, '7', 0, '8', 0, '9', 0, 'A', 0, 'B', 0, 'C', 0
};
