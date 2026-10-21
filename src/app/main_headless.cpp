// Headless runner: guest CPU/OS/devices without graphics, for bring-up and tests.
#include "boot.h"
#include "hw.h"
#include "guest_sched.h"

int main(int argc, char** argv) {
  if (!guest::bootFromArgs(argc, argv)) return 1;
  hw::start();
  sched::runMainThread(guest::g_boot.entry);
  return 0;
}
