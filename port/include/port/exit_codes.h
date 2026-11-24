/* The game's own exit codes, which the launcher's crash page explains (launcher/platform/: describe_exit). */
#ifndef PORT_EXIT_CODES_H
#define PORT_EXIT_CODES_H

/* The GPU stopped answering (the hang watch, port/gs/pc/pcr_hang_watch.h) or its driver reset it (Vulkan's
   VK_ERROR_DEVICE_LOST: a Windows "TDR", a driver update or crash): nothing can be drawn again */
#define PORT_EXIT_GPU_LOST 70

#endif
