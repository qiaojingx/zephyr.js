// Copyright (c) 2017, Intel Corporation.

// Run this test case and run simple TCP server on linux
//     TCP server: /tests/tools/test-socket-server.py
// If using Bluetooth communication, please connect server with Bluetooth
// Add soft router on linux:
//     ip -6 route add 2001:db8::/64 dev bt0
// And add Lan IPv6 address on linux:
//     ip -6 addr add 2001:db8::2/64 dev bt0

console.log("Test socket connection as TCP client for IPv6");

var net = require("net");
var board = require("board");
var assert = require("Assert.js");

var IPv6Address = "2001:db8::2";
var IPv6Port = 9876;
var addressOptions = {
    port: IPv6Port,
    host: IPv6Address,
    localAddress: "2001:db8::1",
    localPort: 8484,
    family: 6
}

var socket = new net.Socket();

assert(typeof socket === "object" && socket !== null,
       "SocketObject: be defined for TCP client");

socket.on("data", function(buf) {
    console.log("receive data: " + buf.toString("ascii"));
});

var SocketconnectFlag = true;
socket.on("connect", function() {
    if (SocketconnectFlag) {
        assert(true, "SocketEvent: be called as 'connect' event");

        SocketconnectFlag = false;
    }

    console.log("socket connection is connected");

    var Count = 0;
    var Timer = setInterval(function() {
        if (Count < 7) {
            socket.write(new Buffer("hello\r\n"));

            console.log("send data: 'hello'");
        }

        if (Count === 7) {
            socket.write(new Buffer("close\r\n"));

            console.log("send data: 'close'");

            assert.result();

            clearInterval(Timer);
        }

        Count = Count + 1;
    }, 3000);
});

socket.on("close", function() {
    console.log("socket connection is closed");
});

var socketonErrorFlag = true;
socket.on("error", function(err) {
    if (socketonErrorFlag) {
        assert(true, "SocketEvent: be called as 'error' event");

        socketonErrorFlag = false;
    }

    console.log("socket connection error: " + err.name);

    if (err.name === "NotFoundError") {
        setTimeout(function() {
            socket.connect(addressOptions, function() {
                assert(true, "SocketObject: connect server successful");
            });
        }, 5000);
    }
});

console.log("If using Bluetooth communication, please connect server with Bluetooth");
console.log("Add soft router on linux:");
console.log("    ip -6 route add 2001:db8::/64 dev bt0");
console.log("And add Lan IPv6 address on linux:");
console.log("    ip -6 addr add 2001:db8::2/64 dev bt0");

if (board.name === "frdm_k64f") {
    console.log("TCP client will be connecting after 30s on K64f board");

    setTimeout(function() {
        socket.connect(addressOptions, function() {
            assert(true, "SocketObject: connect server successful");
        });
    }, 30000);
} else {
    net.on("up", function() {
        assert(true, "NetEvent: be called as 'up' event");
        console.log("TCP client will be connecting after 30s");

        setTimeout(function() {
            socket.connect(addressOptions, function() {
                assert(true, "SocketObject: connect server successful");
            });
        }, 30000);
    });
}
