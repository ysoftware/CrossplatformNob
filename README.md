Build setup for crossplatform SDL-based application

This is an example for building and running any app or game for all platform in C based on SDL3.
Includes files and a build script to get SDL running on these platforms.
Renders a red square on a black background when launched.

Platforms support:
- Linux
- macOS
- Windows (needs testing)
- Android
- iOS (compiled from macOS)

Bootstrap build system
```bash
cc nob.c -o nob
```

Usage and build programme:

1. Native:
- clang or gcc

2. iOS
- Xcode: xcrun, PlistBuddy, ibtool, security, codesign, devicectl, simctl

3. Android
- Java 17 (not higher)
- Android SDK
- Android NDK: javac, jar, d8, aapt2, zip, zipalign, apksigner
- ADB

TODO:
- Think if I should move env to an env.h or directly into nob.c
- Test on Windows
