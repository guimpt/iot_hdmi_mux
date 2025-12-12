# IoT HDMI Mux
## The project
Too lazy to get up from the sofa to change the HDMI cable? Don't want another remote controller lying around? This is your solution!

The IoT HDMI Mux integrates an ESP32-C3, a 2.4GHz antenna and a 2:1 HDMI mux IC in order to create a connected HDMI mux.

![Electronics render](https://github.com/guimpt/iot_hdmi_mux/blob/main/doc/render3.jpg)

## Supported HDMI Features
The project uses the TS3DV642 IC, which is rated for and has been tested to support:
 - HDMI 2.0 compatibility
 - 4K at 60 Hz

Note: The TS3DV642 is a FET-based switch, it is currently unknown whether the switch or PCB implementation may allow performance beyond the listed specifications.

This project is sponsored by www.pcbway.com

<img src="https://github.com/guimpt/iot_hdmi_mux/blob/main/doc/pcbway.png" alt="PCBWay logo" style="width:25%; height:auto;">
