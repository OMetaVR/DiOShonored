# source this: points the cross scripts at the xtool-extracted Xcode 27 SDK (user-local, outside the repo).
XSDK=${XTOOL_DARWIN_SDK:-$HOME/.swiftpm/swift-sdks/darwin.artifactbundle}
export IOS_SDK=${IOS_SDK:-$XSDK/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.1.sdk}
export IOS_LD=${IOS_LD:-$XSDK/toolset/bin/orig/ld64.lld}
export PATH=$HOME/.local/bin:$HOME/.local/share/swiftly/bin:$PATH
