#ifndef USB_DSD_OUTPUT_H
#define USB_DSD_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct usb_dsd_output usb_dsd_output_t;

/* Opens the USB PCM device directly. dop_rate is the DoP frame rate (DSD bit
 * rate / 16); native mode derives the ALSA rate from the selected word size. */
usb_dsd_output_t * usb_dsd_output_open(unsigned int card, unsigned int device,
                                        bool native, unsigned int channels,
                                        unsigned int dop_rate);
bool usb_dsd_output_write(usb_dsd_output_t * output, const int32_t * dop_words,
                          uint64_t frames, uint64_t * frames_written);
void usb_dsd_output_close(usb_dsd_output_t * output);
bool usb_dsd_output_is_native(const usb_dsd_output_t * output);
unsigned int usb_dsd_output_hardware_rate(const usb_dsd_output_t * output);
unsigned int usb_dsd_output_hardware_bits(const usb_dsd_output_t * output);

#endif
