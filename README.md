# Hydra IPC Library

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

## Overview
Hydra is a lightweight, high-performance cross-process communication (IPC) and synchronization library for C++20. It operates as a seamless "hivemind," allowing different processes to rapidly share and synchronize data states with minimal overhead as well as directly communicate with Asynchronous Procedure Calls (APCs) being used to near instantly inform processes of pending commands to process.

It features Asynchronous Procedure Calls (APCs) for near instant thread signaling, Seqlocks for state reading, and a Leader/Follower dynamic. If the leader process terminates, Hydra can automatically promote a follower into the leader role for painless handling across different scenarios.

## Build Instructions
This library is meant to be used via the 2 provided headers by first cloning it, then including said headers in whichever project they are needed. The library was developed and tested using solely x64 and the MSVC compiler from Visual Studio 2026.

## Usage
A Proof Of Concept can be seen in the following repository: [Hydra IPC PoC](https://github.com/DIA4A/Hydra-RotMG-Exalt-Multibox)

## License
This project is licensed under the [MIT License](LICENSE).