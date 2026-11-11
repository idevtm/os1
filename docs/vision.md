# OS1 Vision

This document describes a future direction for OS1.
OS1 aims to make a computer understandable and controllable, from small embedded devices to graphical computers.
This vision is a proposal, not a claim that all features exist.

```text
                  Shared system contracts
        discovery | observation | explicit authority
                 /         |          \
             Core       Desktop      Embedded
             shell       GUI          device interface
```

The [project goals](../GOALS.md) define the educational scope and priorities.
The [architecture](ARCHITECTURE.md) describes current behavior.
The [Native API index](native-api/README.md) separates implemented interfaces from plans and proposals.

## One understandable system

OS1 should help people understand what their computer does and why.
Users should discover devices, services, and processes through consistent system objects.
They should inspect relationships, capabilities, events, and failures without parsing unrelated text formats.
The source should remain clear enough to study, change, and test.

Programs should receive explicit authority over the resources they need.
A user should be able to understand which keyboard, display, or other resource a program can access.
A failure should retain useful evidence and lead to a clear diagnostic path.

## Shell and graphical interface

The shell and graphical interface should present the same underlying system concepts.
The shell should provide precise inspection and scripting.
The graphical interface should make discovery and relationships easier to explore.

A future hardware explorer could show a device, its connection path, driver, capabilities, and recent failures.
A user could inspect the same information through the shell.
A process view could show held resources and explain waits when the system has sufficient evidence.
These examples describe desired experiences, not current guarantees.

## A family of configurations

| Working name | Intended experience | Shared foundation |
| --- | --- | --- |
| Core | A small system for learning, development, and shell diagnostics | Kernel, native interfaces, discovery, observation, and explicit authority |
| Desktop | A graphical environment for applications and system exploration | Core contracts with graphical services |
| Embedded | A tailored device or appliance with an optional display | Core contracts with only the required services |

These names describe ideas, not established products or separate implementations.
Each configuration should share concepts and contracts where practical.
A configuration should be able to omit services that it does not need.
An embedded device should not require a window system.
A desktop should retain a useful graphical interface.

An embedded device could expose component status and bounded diagnostic events through a suitable local interface.
A future remote management interface could provide the same information when its security and transport designs are ready.
It is not an implemented capability.

## A first demonstration

A future demonstration could boot a simple graphical desktop and show a USB mouse in a hardware explorer.
The user could inspect the driver and live events, then disconnect the mouse and see its state change.
The shell could provide the same inspection.
Existing input, discovery, and diagnostics work provides foundations for this experience.
The complete demonstration and its physical reliability require further implementation and validation.

A later screenless configuration could expose equivalent device information through a small diagnostic interface.
The [service roadmap](native-api/roadmap.md) records current dependencies and priorities.
