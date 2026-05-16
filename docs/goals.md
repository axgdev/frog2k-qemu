Completely simulate SF2000 to have an oracle/smoke testing software we can work with to quickly iterate.
To achieve that this Qemu port must be fast and accurate and have good tooling.
There must be ways to skip steps when are not interested in them.
That can only be possible if we have modeled those steps so well that it will always have a state that is realistic.
Or we could possibly just save snapshots at different points and continue from there for particular tests.
Ultimately this emulator will be so good that it will be easy to port any OS and software to the SF2000 just by using this QEMU port.
