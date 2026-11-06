# OhMyDSP sources

See the [top-level README](../README.md) for what this is, the architecture and how to install it.

    make CORE=1                # kext (Lilu plugin) -> build/core-lilu/OhMyDSP.kext, for OpenCore Kernel > Add (after Lilu)
    make CORE=1 LILU=0         # standalone, for sudo sh ../tools/load-kext.sh core
    make                       # legacy IOAudioFamily variant (build/lilu, build/standalone); cannot be injected by OpenCore
    make -C userspace          # ohmydspd + OhMyDSP.driver
    make -C prefpane           # OhMyDSP.prefPane

Layout: `src/` kext, `include/` kernel/user contract, `userspace/` daemon and AudioServerPlugIn, `prefpane/` System Settings pane,
`third_party/Lilu` vendored SDK headers (BSD-3-Clause).
