# Chrono Rift

## Overview

Chrono Rift is a turn-based tactical RPG developed as part of an Operating Systems course project. The game combines classic RPG combat mechanics with advanced OS concepts including multiprocessing, multithreading, shared memory communication, synchronization primitives, deadlock detection, process signaling, and scheduling algorithms.

The project simulates a real-time battle environment where player-controlled characters and AI-controlled enemies compete for turns based on a stamina-driven scheduling system.

---

## Features

### Turn-Based Combat System

* Dynamic stamina-based scheduling
* Multiple player-controlled characters
* AI-controlled enemy units
* Health, damage, speed, and stamina attributes
* Multiple combat actions and abilities

### Operating System Concepts

#### Multiprocessing

* Game Arbiter Process
* Human Interface Process
* Automated Strategic Process

#### Multithreading

* Dedicated thread for each player character
* Dedicated thread for each NPC enemy
* Separate rendering thread for UI updates
* Background deadlock monitoring thread

#### Inter-Process Communication

* Shared Memory based communication
* No pipes used
* Memory-based synchronization primitives

#### Synchronization

* Mutexes
* Semaphores
* Shared resource protection
* Race condition prevention

#### Signals

* Stun mechanic implementation
* Ultimate ability pause system
* Process suspension and resumption
* Graceful termination handling

#### Deadlock Detection

* Resource allocation tracking
* Circular wait detection
* Automatic deadlock recovery

---

## Core Gameplay Mechanics

### Stamina-Based Scheduling

Each entity accumulates stamina according to its speed. Once maximum stamina is reached, the entity becomes eligible to perform an action.

### Stun System

Powerful attacks can stun targets for 3 seconds using asynchronous signal-based interruption.

### Inventory Management

* 20-slot inventory system
* Contiguous memory allocation
* Weapon swapping
* Long-term storage management
* Inventory fragmentation handling

### Artifact System

Special artifacts include:

* Solar Core
* Lunar Blade
* Eclipse Relic

Players must manage shared resources while avoiding deadlocks.

### Ultimate Ability

A special ability requiring both Solar Core and Lunar Blade. Activating it suspends all enemy AI for 10 seconds using signals.

---

## Weapons

| Weapon         | Slots | Damage |
| -------------- | ----- | ------ |
| Solar Core     | 10    | 95     |
| Lunar Blade    | 10    | 90     |
| Iron Halberd   | 7     | 55     |
| Venom Dagger   | 4     | 30     |
| Thunderstaff   | 6     | 50     |
| Obsidian Axe   | 5     | 45     |
| Frostbow       | 6     | 48     |
| Splinter Stick | 2     | 12     |

---

## Game Architecture

### Game Arbiter

* Maintains global game state
* Handles scheduling
* Enforces game rules
* Monitors deadlocks
* Coordinates communication

### Human Interface Process

* Receives player input
* Manages player threads
* Sends actions to Arbiter

### Automated Strategic Process

* Controls enemy AI
* Runs NPC threads
* Generates combat decisions

---

## Win Conditions

The game ends when:

### Victory

* Players defeat 10 enemies

### Defeat

* All player characters are eliminated

### Quit

* Player exits the game

---

## Technologies Used

* C++
* POSIX Threads (pthreads)
* Shared Memory
* Mutexes
* Semaphores
* Signals
* Linux System Calls
* Docker
* ncurses / GUI Framework

---

## Learning Outcomes

This project demonstrates practical implementation of:

* Process Management
* Thread Management
* CPU Scheduling Concepts
* Inter-Process Communication
* Synchronization Mechanisms
* Deadlock Detection and Recovery
* Signal Handling
* Memory Management
* Concurrent Programming

---

## Authors

Developed as an Operating Systems Semester Project.
