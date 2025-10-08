Build setup for crossplatform SDL-based application

Platforms support:
- Linux
- macOS
- Windows (needs testing)
- Android (crosscompilation)
- iOS (compiled from macOS)

Dependencies:
- SDL-3.1.26 included in the project
- Android Studio -- expand on which executables exactly
- Xcode

Bootstrap build system
```bash
cc nob.c -o nob
```

Usage and build programme:

1. Native:

2. iOS

3. Android
- Java 17
- Android SDK
- Android NDK

TODO:
- Think if I should move env to an env.h or directly into nob.c
- Test on Windows
- Test for Android Device
- Put all env in 1 file 
- Mention build times for mobile platforms and how nice it is compared to default garbage tools
- Include Xcode macOS project for debugging
- Include step of building and using objc code 
