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
./mqhttp localhost 1883
```

After starting, HTTP server listens for incoming connections on localhost, port 8080.
If you are not happy with these defaults, you can change them by adjusting environment variables:
```
export HTTP_PORT=8080
export HTTP_ADDR=0.0.0.0
```

During the start up, MQhTTp sources all the lua files in the current directory (if any).

## Bugs

Yes.
