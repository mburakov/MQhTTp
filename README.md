# MQhTTp

This is a lightweight lua-scriptable http gateway to mosquitto.

## Building on Linux

MQhTTp depends on libmosquitto and luajit. Once you have these installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own.

## Running

Provide host and port of your mosquitto server on the commandline, i.e.
```
./mqhttp 127.0.0.1 1883
```

After starting, HTTP server listens for incoming connections on localhost, port 8080.
If you are not happy with these defaults, you can change them by adjusting environment variables:
```
export HTTP_PORT=8080
export HTTP_ADDR=0.0.0.0
```

During the start up, MQhTTp sources all the lua files in the current directory (if any).

## Usage

You can use HTTP GET requests to browse through the topics known to your mosquitto server, and get their last received payloads, i.e.:
```
# curl -v rock:8080/
*   Trying 192.168.8.6:8080...
* Connected to rock (192.168.8.6) port 8080 (#0)
> GET / HTTP/1.1
> Host: rock:8080
> User-Agent: curl/7.79.1
> Accept: */*
> 
* Mark bundle as not supporting multiuse
< HTTP/1.1 200 OK
< Content-Type: text/html
< Content-Length: 1770
< 
* Connection #0 to host rock left intact
<!DOCTYPE html><html><head><title>MQhTTp</title></head><body><a href="/cmnd/bedroom/Color">/cmnd/bedroom/Color<br><a href="/cmnd/torsh/Color">/cmnd/torsh/Color<br><a href="/stat/bedroom/RESULT">/stat/bedroom/RESULT<br><a href="/stat/torsh/RESULT">/stat/torsh/RESULT<br><a href="/tasmota/discovery/3C71BF25D0EE/config">/tasmota/discovery/3C71BF25D0EE/config<br><a href="/tasmota/discovery/3C71BF25D0EE/sensors">/tasmota/discovery/3C71BF25D0EE/sensors<br><a href="/tasmota/discovery/68C63AEC65D0/config">/tasmota/discovery/68C63AEC65D0/config<br><a href="/tasmota/discovery/68C63AEC65D0/sensors">/tasmota/discovery/68C63AEC65D0/sensors<br><a href="/tele/bedroom/LWT">/tele/bedroom/LWT<br><a href="/tele/bedroom/STATE">/tele/bedroom/STATE<br><a href="/tele/torsh/LWT">/tele/torsh/LWT<br><a href="/tele/torsh/STATE">/tele/torsh/STATE<br><a href="/zigbee2mqtt/bridge/config">/zigbee2mqtt/bridge/config<br><a href="/zigbee2mqtt/bridge/devices">/zigbee2mqtt/bridge/devices<br><a href="/zigbee2mqtt/bridge/extensions">/zigbee2mqtt/bridge/extensions<br><a href="/zigbee2mqtt/bridge/groups">/zigbee2mqtt/bridge/groups<br><a href="/zigbee2mqtt/bridge/info">/zigbee2mqtt/bridge/info<br><a href="/zigbee2mqtt/bridge/log">/zigbee2mqtt/bridge/log<br><a href="/zigbee2mqtt/bridge/logging">/zigbee2mqtt/bridge/logging<br><a href="/zigbee2mqtt/bridge/state">/zigbee2mqtt/bridge/state<br><a href="/zigbee2mqtt/kitchen">/zigbee2mqtt/kitchen<br><a href="/zigbee2mqtt/kitchen/set">/zigbee2mqtt/kitchen/set<br><a href="/zigbee2mqtt/lamps">/zigbee2mqtt/lamps<br><a href="/zigbee2mqtt/lamps/set">/zigbee2mqtt/lamps/set<br><a href="/zigbee2mqtt/leds">/zigbee2mqtt/leds<br><a href="/zigbee2mqtt/leds/set">/zigbee2mqtt/leds/set<br><a href="/zigbee2mqtt/switch">/zigbee2mqtt/switch<br></body></html>
```
```
# curl -v http://rock:8080/tele/torsh/STATE
*   Trying 192.168.8.6:8080...
* Connected to rock (192.168.8.6) port 8080 (#0)
> GET /tele/torsh/STATE HTTP/1.1
> Host: rock:8080
> User-Agent: curl/7.79.1
> Accept: */*
> 
* Mark bundle as not supporting multiuse
< HTTP/1.1 200 OK
< Content-Type: application/json
< Content-Length: 435
< 
* Connection #0 to host rock left intact
{"Time":"2021-11-01T15:07:51","Uptime":"28T17:32:14","UptimeSec":2482334,"Heap":27,"SleepMode":"Dynamic","Sleep":50,"LoadAvg":19,"MqttCount":8,"POWER":"OFF","Dimmer":0,"Color":"0,0,0,0","HSBColor":"0,0,0","White":0,"Channel":[0,0,0,0],"Scheme":0,"Fade":"OFF","Speed":1,"LedTable":"ON","Wifi":{"AP":1,"SSId":"UPCB84945E","BSSId":"C4:AD:34:7D:9C:73","Channel":3,"Mode":"11n","RSSI":70,"Signal":-65,"LinkCount":1,"Downtime":"0T00:00:03"}}
```

MQhTTp would publish to mosquitto anything that arrives as an HTTP POST request using URL as a topic (omitting leading slash) and request body as a payload, i.e.:
```
# curl -d '#00000000' -v rock:8080/cmnd/torsh/Color
*   Trying 192.168.8.6:8080...
* Connected to rock (192.168.8.6) port 8080 (#0)
> POST /cmnd/torsh/Color HTTP/1.1
> Host: rock:8080
> User-Agent: curl/7.79.1
> Accept: */*
> Content-Length: 9
> Content-Type: application/x-www-form-urlencoded
> 
* Mark bundle as not supporting multiuse
< HTTP/1.1 200 OK
< Content-Length: 0
< 
* Connection #0 to host rock left intact
```

## Bugs

Yes.
