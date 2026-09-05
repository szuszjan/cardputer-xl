// setup()/loop() and all app code live in CardputerXL.cpp, compiled
// alongside this file as part of the same sketch.
//
// Why a .cpp instead of a normal .ino: Arduino's .ino build step
// auto-generates forward declarations for every function and inserts them
// above this sketch's own struct definitions (KVec3, KartCam, ...), which
// breaks the build - see the comment near KVec3 in CardputerXL.cpp. A .cpp
// file is compiled as an ordinary top-to-bottom translation unit instead,
// with no such insertion, so it just works.
