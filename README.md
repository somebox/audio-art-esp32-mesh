# Audio Art Installation

> ESP32 devices connected via a mesh network, which are synchronized for an audio performance. The subject is a conversation about art between a real artist, Edward Wright, and the GPT-3 AI model from OpenAI. This exhibition was held in Luzern, Switzerland in February 2022.

![overview](https://github.com/user-attachments/assets/ed0aa335-646f-4f82-90ed-5623edccc0ba)

## Overview

A number of ESP32 nodes are placed in the same room, connected via a wireless mesh network. Each node has a different role in the performance, and coordiate to play specific audio samples, send signals to a synth module, or turn lights off and on. Some of the events are sequenced and orchestrated, others are random.

The networking is based on PainlessMesh, and requires no WIFI access point to work - the nodes will create a mesh and self-configure themselves. Each node has a unique name, and listens for messages it can respond to. A single node acts as a conductor, following a series of audio files to play in sequence (questions from the artist, and answers from GPT-3 AI).

The audio is handled by the I2SAudio library, and samples are played from an SD card. Other connections are made via gate or CV signals, and lights and devices are controlled by triggering relays or using PWM-controlled mosfets.

The performance given defines a total of 6 nodes: two audio players, three synth controllers, and a single coordinator/conductor. Several nodes had speakers connected, or lights/LEDs.

The MP3 audio files were generated with the Amazon AWS "Polly" text-to-speech service, fed from a list of GPT-3's answers stored in a plaintext file, and rendered with [this simple Python script](https://github.com/somebox/aws-polly-python-example).

## Flow Diagram

```
              ╔══════════════════════════════╗               
              ║          STATE FLOW          ║               
              ╚══════════════════════════════╝               
                                                             
     ┌────────────────────────────────────────────────┐      
     │                                                │      
     ▼             ┌────────────┐     ┌────────────┐  │      
┌─────────┐        │            │     │            │  │      
│         │     ┌─▶│  Question  │  ┌─▶│   Answer   │  │      
│  Start  │     │  │            │  │  │            │  │      
│         │     │  └────────────┘  │  └────────────┘  │      
└─────────┘     │         │        │         │        │      
     │   ┌────────────┐   │ ┌────────────┐   │ ┌────────────┐
     └──▶│   Pause    │   └▶│   Pause    │   └▶│   Pause    │
         └────────────┘     └────────────┘     └────────────┘
```
In the Start state, the current cycle is initialized. The question counter is incremented, and a pause is started on the controller. The first question is triggered, and after it is completed, another pause begins. Finally the answer is triggered, folled by a final pause, and the cycle begins again.

## Hardware

There are three ESP32 devices on the mesh which serve different roles: 

```
     ╔══════════════════════════════╗      
     ║    Network: ESP32 (Mesh)     ║      
     ╚══════════════════════════════╝      
                 ┌ ─ ─ ─ ┐       ┌ ─ ─ ─ ┐ 
                   AUDIO           AUDIO   
                 └ ─ ─ ─ ┘       └ ─ ─ ─ ┘ 
                     ▲               ▲     
 ┌──┬─┬──┐       ┌──┬┴┬──┐       ┌──┬┴┬──┐ 
 │┌┐└─┘┌┐│       │┌┐└─┘┌┐│       │┌┐└─┘┌┐│ 
 │└┘▬▮▬└┘│       │└┘▬▮▬└┘│       │└┘▬▮▬└┘│ 
 │▮▮◐ ▬ ◌│       │▮▮◐ ▬ ◌│       │▮▮◐ ▬ ◌│ 
 │┌─────┐│       │┌─────┐│       │┌─────┐│ 
 ││     ││       ││     ││       ││     ││ 
 ││     ││       ││     ││       ││     ││ 
 ││ESP32││       ││ESP32││       ││ESP32││ 
 ││     ││       ││     ││       ││     ││ 
 └┴─────┴┘       └┴─────┴┘       └┴─────┴┘ 
Controller       Questions        Answers  
```

* Controller node: Orchestrates the performance, the order of questions, the pauses, and other parameters.
* Question node: Plays back mp3 samples for the pre-recorded human questions.
* Answer node: Plays back mp3 samples for the GPT-3 speech-synthsized answers.

Pauses may be random in length. Using the wireless mesh, events are published, such as triggering mp3 playback, or 
notifying when that track is finished. 
## Installation

Each node runs the same code, and will act depending on the role selected. When uploading firmware to the ESP32 boards, it is neccessary to change the `NODE_ROLE` value to one of the recognized names. 

