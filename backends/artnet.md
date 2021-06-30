### The `artnet` backend

The ArtNet backend provides read-write access to the UDP-based ArtNet protocol for lighting
fixture control.

Art-Net™ Designed by and Copyright Artistic Licence Holdings Ltd.

#### Global configuration

| Option	| Example value		| Default value 	| Description		|
|---------------|-----------------------|-----------------------|-----------------------|
| `bind`	| `127.0.0.1 6454`	| none			| Binds a network address to listen for data. This option may be set multiple times, with each interface being assigned an index starting from 0 to be used with the `interface` instance configuration option. At least one interface is required for transmission. |
| `net`		| `0`			| `0`			| The default net to use |
| `detect`	| `on`, `verbose`	| `off`			| Output additional information on received data packets to help with configuring complex scenarios |

#### Instance configuration

| Option	| Example value		| Default value 	| Description		|
|---------------|-----------------------|-----------------------|-----------------------|
| `net`		| `0`			| `0`			| ArtNet `net` to use	|
| `universe`	| `0`			| `0`			| Universe identifier	|
| `destination`	| `10.2.2.2`		| none			| Destination address for sent ArtNet frames. Setting this enables the universe for output |
| `interface`	| `1`			| `0`			| The bound address to use for data input/output |
| `realtime`	| `1`			| `0`			| Disable the recommended rate-limiting (approx. 44 packets per second) for this instance |

#### Channel specification

A channel is specified by it's universe index. Channel indices start at 1 and end at 512.

Example mapping:
```
net1.231 < net2.123
```

A 16-bit channel (spanning any two normal 8-bit channels in the same universe, also called a wide channel) may be mapped with the syntax
```
net1.1+2 > net2.5+123
```

A normal channel that is part of a wide channel can not be mapped individually.

#### Known bugs / problems

When using this backend for output with a fast event source, some events may appear to be lost due to the packet output rate limiting
mandated by the [ArtNet specification](https://artisticlicence.com/WebSiteMaster/User%20Guides/art-net.pdf) (Section `Refresh rate`).
This limit can be disabled on a per-instance basis using the `realtime` instance option.
