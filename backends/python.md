### The `python` backend

The `python` backend provides a flexible programming environment, allowing users
to route, generate and manipulate channel events using the Python 3 scripting language.

Every instance has its own interpreter, which can be loaded with multiple Python modules.
These modules may contain member functions accepting a single `float` parameter, which can
then be used as target channels. For each incoming event, the handler function is called.
Channels in the global scope may be assigned a default handler function.

Python modules may also register `socket` objects (and an associated callback function) with
the MIDIMonster core, which will then alert the module when there is data ready to be read.

To interact with the MIDIMonster core, import the `midimonster` module from within your module.

The `midimonster` module provides the following functions:

| Function			| Usage example				| Description					|
|-------------------------------|---------------------------------------|-----------------------------------------------|
| `output(string, float)`	| `midimonster.output("foo", 0.75)`	| Output a value event to a channel on this instance |
| `inputvalue(string)`		| `midimonster.inputvalue("foo")`	| Get the last input value on a channel	of this instance |
| `outputvalue(string)`		| `midimonster.outputvalue("bar")`	| Get the last output value on a channel of this instance |
| `current()`			| `print(midimonster.current())`	| Returns the name of the input channel whose handler function is currently running or `None` if the interpreter was called from another context |
| `timestamp()`			| `print(midimonster.timestamp())`	| Get the internal core timestamp (in milliseconds)	|
| `interval(function, long)`	| `midimonster.interval(toggle, 100)`	| Register a function to be called periodically. Interval is specified in milliseconds (accurate to 10msec). Calling `interval` with the same function again updates the interval. Specifying the interval as `0` cancels the interval |
| `manage(function, socket)`	| `midimonster.manage(handler, socket)`	| Register a (connected/listening) socket to the MIDIMonster core. Calls `function(socket)` when the socket is ready to read. Calling this method with `None` as the function argument unregisters the socket. A socket may only have one associated handler |
| `channels()`			| `midimonster.channels()`		| Fetch a list of all currently known channels on the instance. Note that this function only returns useful data after the configuration has been read completely, i.e. any time after initial startup |
| `cleanup_handler(function)`	| `midimonster.cleanup_handler(save_all)`| Register a function to be called when the instance is destroyed (on MIDIMonster shutdown). One cleanup handler can be registered per instance. Calling this function when the instance already has a cleanup handler registered replaces the handler, returning the old one. |

When a channel handler executes, calling `midimonster.inputvalue()` for that exact channel returns the previous value,
while the argument to the handler is the current value. The stored value is updated after the handler finishes executing.

Example Python module:
```python
import socket
import midimonster

# Simple channel handler
def in1(value):
	midimonster.output("out1", 1 - value)

# Socket data handler
def socket_handler(sock):
	# This should get some more error handling
	data = sock.recv(1024)
	print("Received %d bytes from socket: %s" % (len(data), data))
	if(len(data) == 0):
		# Unmanage the socket if it has been closed
		midimonster.manage(None, sock)
		sock.close()

# Interval handler
def ping():
	print(midimonster.timestamp())

def save_positions():
	# Store some data to disk

# Register an interval
midimonster.interval(ping, 1000)
# Create and register a client socket (add error handling as you like)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 8990))
midimonster.manage(socket_handler, s)
midimonster.cleanup_handler(save_positions)
```

Input values range between 0.0 and 1.0, output values are clamped to the same range.

Note that registered sockets that have been closed (`socket.recv()` returned 0 bytes)
need to be unregistered from the MIDIMonster core, otherwise the core socket multiplexing
mechanism will report an error and shut down the MIDIMonster.

#### Global configuration

The `python` backend does not take any global configuration.

#### Instance configuration

| Option		| Example value		| Default value 	| Description					|
|-----------------------|-----------------------|-----------------------|-----------------------------------------------|
| `module`		| `my_handlers`		| none			| Name of the python module to load (normally the name of a`.py` file without the extension) |
| `default-handler`	| `my_handlers.default`	| none			| Function to be called as handler for all top-level channels (not belonging to a module) |

A single instance may have multiple `module` options specified. This will make all handlers available within their
module namespaces (see the section on channel specification).

#### Channel specification

Channel names may be any valid Python function name. To call handler functions in a module,
specify the channel as the functions qualified path (by prefixing it with the module name and a dot).

Example mappings:
```
py1.my_handlers.in1 < py1.foo
py1.out1 > py2.module.handler
```

#### Known bugs / problems

Output values will not trigger corresponding input event handlers unless the channel is mapped
back in the MIDIMonster configuration. This is intentional.

Output events generated from cleanup handlers called during shutdown will not be routed, as the core
routing facility has already shut down at this point. There are no plans to change this behaviour.

Importing a Python module named `midimonster` is probably a bad idea and thus unsupported.

The MIDIMonster is, at its core, single-threaded. Do not try to use Python's `threading`
module with the MIDIMonster.

Note that executing Python code blocks the MIDIMonster core. It is not a good idea to call functions that
take a long time to complete (such as `time.sleep()`) within your Python modules.
