// Host stand-in for src/libs/Kernel.h: just enough for src/libs/PublicData.cpp
// to compile on the host. run.sh puts this directory ahead of src/ on the
// include path, so PublicData.cpp's #include "libs/Kernel.h" finds this file.
// The test defines Kernel::call_event() to answer the way Player does.
#ifndef KERNEL_H
#define KERNEL_H

#include <cstddef>
#include <cstdint>

#include "libs/Module.h"

#define THEKERNEL Kernel::instance

class Kernel {
 public:
  static Kernel* instance;
  void call_event(_EVENT_ENUM id_event, void* argument = nullptr);
};

#endif
