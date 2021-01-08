#define BACKEND_NAME "winmidi"

#include <string.h>

#include "libmmbackend.h"
#include <mmsystem.h>

#include "winmidi.h"

static struct {
	uint8_t list_devices;
	uint8_t detect;
	int socket_pair[2];

	CRITICAL_SECTION push_events;
	volatile size_t events_alloc;
	volatile size_t events_active;
	volatile winmidi_event* event;
} backend_config = {
	.list_devices = 0,
	.socket_pair = {-1, -1}
};

//TODO receive feedback socket until EAGAIN

MM_PLUGIN_API int init(){
	backend winmidi = {
		.name = BACKEND_NAME,
		.conf = winmidi_configure,
		.create = winmidi_instance,
		.conf_instance = winmidi_configure_instance,
		.channel = winmidi_channel,
		.handle = winmidi_set,
		.process = winmidi_handle,
		.start = winmidi_start,
		.shutdown = winmidi_shutdown
	};

	if(sizeof(winmidi_channel_ident) != sizeof(uint64_t)){
		LOG("Channel identification union out of bounds");
		return 1;
	}

	//register backend
	if(mm_backend_register(winmidi)){
		LOG("Failed to register backend");
		return 1;
	}

	//initialize critical section
	InitializeCriticalSectionAndSpinCount(&backend_config.push_events, 4000);
	return 0;
}

static int winmidi_configure(char* option, char* value){
	if(!strcmp(option, "list")){
		backend_config.list_devices = 0;
		if(!strcmp(value, "on")){
			backend_config.list_devices = 1;
		}
		return 0;
	}
	else if(!strcmp(option, "detect")){
		backend_config.detect = 0;
		if(!strcmp(value, "on")){
			backend_config.detect = 1;
		}
		return 0;
	}

	LOGPF("Unknown backend option %s", option);
	return 1;
}

static int winmidi_configure_instance(instance* inst, char* option, char* value){
	winmidi_instance_data* data = (winmidi_instance_data*) inst->impl;
	if(!strcmp(option, "read")){
		if(data->read){
			LOGPF("Instance %s already connected to an input device", inst->name);
			return 1;
		}
		data->read = strdup(value);
		return 0;
	}
	else if(!strcmp(option, "write")){
		if(data->write){
			LOGPF("Instance %s already connected to an output device", inst->name);
			return 1;
		}
		data->write = strdup(value);
		return 0;
	}
	else if(!strcmp(option, "epn-tx")){
		data->epn_tx_short = 0;
		if(!strcmp(value, "short")){
			data->epn_tx_short = 1;
		}
		return 0;
	}

	LOGPF("Unknown instance configuration option %s on instance %s", option, inst->name);
	return 1;
}

static int winmidi_instance(instance* inst){
	inst->impl = calloc(1, sizeof(winmidi_instance_data));
	if(!inst->impl){
		LOG("Failed to allocate memory");
		return 1;
	}

	return 0;
}

static channel* winmidi_channel(instance* inst, char* spec, uint8_t flags){
	char* next_token = NULL;
	winmidi_channel_ident ident = {
		.label = 0
	};

	if(!strncmp(spec, "ch", 2)){
		next_token = spec + 2;
		if(!strncmp(spec, "channel", 7)){
			next_token = spec + 7;
		}
	}

	if(!next_token){
		LOGPF("Invalid channel specification %s", spec);
		return NULL;
	}

	ident.fields.channel = strtoul(next_token, &next_token, 10);
	if(ident.fields.channel > 15){
		LOGPF("MIDI channel out of range in spec %s", spec);
		return NULL;
	}

	if(*next_token != '.'){
		LOGPF("Channel specification %s does not conform to channel<X>.<control><Y>", spec);
		return NULL;
	}

	next_token++;

	if(!strncmp(next_token, "cc", 2)){
		ident.fields.type = cc;
		next_token += 2;
	}
	else if(!strncmp(next_token, "note", 4)){
		ident.fields.type = note;
		next_token += 4;
	}
	else if(!strncmp(next_token, "pressure", 8)){
		ident.fields.type = pressure;
		next_token += 8;
	}
	else if(!strncmp(next_token, "rpn", 3)){
		ident.fields.type = rpn;
		next_token += 3;
	}
	else if(!strncmp(next_token, "nrpn", 4)){
		ident.fields.type = nrpn;
		next_token += 4;
	}
	else if(!strncmp(next_token, "pitch", 5)){
		ident.fields.type = pitchbend;
	}
	else if(!strncmp(next_token, "aftertouch", 10)){
		ident.fields.type = aftertouch;
	}
	else{
		LOGPF("Unknown control type in %s", spec);
		return NULL;
	}

	ident.fields.control = strtoul(next_token, NULL, 10);

	if(ident.label){
		return mm_channel(inst, ident.label, 1);
	}
	return NULL;
}

static void winmidi_tx(HMIDIOUT port, uint8_t type, uint8_t channel, uint8_t control, uint16_t value){
	union {
		struct {
			uint8_t status;
			uint8_t data1;
			uint8_t data2;
			uint8_t unused;
		} components;
		DWORD dword;
	} output = {
		.dword = 0
	};

	output.components.status = type | channel;
	output.components.data1 = control;
	output.components.data2 = value;

	if(type == pitchbend){
		output.components.data1 = value & 0x7F;
		output.components.data2 = (value >> 7) & 0x7F;
	}
	else if(type == aftertouch){
		output.components.data1 = value;
		output.components.data2 = 0;
	}

	midiOutShortMsg(port, output.dword);
}

static int winmidi_set(instance* inst, size_t num, channel** c, channel_value* v){
	winmidi_instance_data* data = (winmidi_instance_data*) inst->impl;
	winmidi_channel_ident ident = {
		.label = 0
	};
	size_t u;

	if(!data->device_out){
		LOGPF("Instance %s has no output device", inst->name);
		return 0;
	}

	for(u = 0; u < num; u++){
		ident.label = c[u]->ident;

		switch(ident.fields.type){
			case rpn:
			case nrpn:
				//transmit parameter number
				winmidi_tx(data->device_out, cc, ident.fields.channel, (ident.fields.type == rpn) ? 101 : 99, (ident.fields.control >> 7) & 0x7F);
				winmidi_tx(data->device_out, cc, ident.fields.channel, (ident.fields.type == rpn) ? 100 : 98, ident.fields.control & 0x7F);

				//transmit parameter value
				winmidi_tx(data->device_out, cc, ident.fields.channel, 6, (((uint16_t) (v[u].normalised * 16383.0)) >> 7) & 0x7F);
				winmidi_tx(data->device_out, cc, ident.fields.channel, 38, ((uint16_t) (v[u].normalised * 16383.0)) & 0x7F);

				if(!data->epn_tx_short){
					//clear active parameter
					winmidi_tx(data->device_out, cc, ident.fields.channel, 101, 127);
					winmidi_tx(data->device_out, cc, ident.fields.channel, 100, 127);
				}
				break;
			case pitchbend:
				winmidi_tx(data->device_out, ident.fields.type, ident.fields.channel, ident.fields.control, v[u].normalised * 16383.0);
				break;
			default:
				winmidi_tx(data->device_out, ident.fields.type, ident.fields.channel, ident.fields.control, v[u].normalised * 127.0);
		}
	}

	return 0;
}

static char* winmidi_type_name(uint8_t typecode){
	switch(typecode){
		case note:
			return "note";
		case cc:
			return "cc";
		case rpn:
			return "rpn";
		case nrpn:
			return "nrpn";
		case pressure:
			return "pressure";
		case aftertouch:
			return "aftertouch";
		case pitchbend:
			return "pitch";
	}
	return "unknown";
}

static int winmidi_handle(size_t num, managed_fd* fds){
	size_t u;
	ssize_t bytes = 0;
	char recv_buf[1024];
	channel* chan = NULL;
	if(!num){
		return 0;
	}

	//flush the feedback socket
	for(u = 0; u < num; u++){
		bytes += recv(fds[u].fd, recv_buf, sizeof(recv_buf), 0);
	}

	//push queued events
	EnterCriticalSection(&backend_config.push_events);
	for(u = 0; u < backend_config.events_active; u++){
		if(backend_config.detect){
			//pretty-print channel-wide events
			if(backend_config.event[u].channel.fields.type == pitchbend
					|| backend_config.event[u].channel.fields.type == aftertouch){
				LOGPF("Incoming data on channel %s.ch%d.%s, value %f",
						backend_config.event[u].inst->name,
						backend_config.event[u].channel.fields.channel,
						winmidi_type_name(backend_config.event[u].channel.fields.type),
						backend_config.event[u].value.normalised);
			}
			else{
				LOGPF("Incoming data on channel %s.ch%d.%s%d, value %f",
						backend_config.event[u].inst->name,
						backend_config.event[u].channel.fields.channel,
						winmidi_type_name(backend_config.event[u].channel.fields.type),
						backend_config.event[u].channel.fields.control,
						backend_config.event[u].value.normalised);
			}
		}
		chan = mm_channel(backend_config.event[u].inst, backend_config.event[u].channel.label, 0);
		if(chan){
			mm_channel_event(chan, backend_config.event[u].value);
		}
	}
	DBGPF("Flushed %" PRIsize_t " wakeups, handled %" PRIsize_t " events", bytes, backend_config.events_active);
	backend_config.events_active = 0;
	LeaveCriticalSection(&backend_config.push_events);
	return 0;
}

static int winmidi_enqueue_input(instance* inst, winmidi_channel_ident ident, channel_value val){
	EnterCriticalSection(&backend_config.push_events);
	if(backend_config.events_alloc <= backend_config.events_active){
		backend_config.event = realloc((void*) backend_config.event, (backend_config.events_alloc + 1) * sizeof(winmidi_event));
		if(!backend_config.event){
			LOG("Failed to allocate memory");
			backend_config.events_alloc = 0;
			backend_config.events_active = 0;
			LeaveCriticalSection(&backend_config.push_events);
			return 1;
		}
		backend_config.events_alloc++;
	}
	backend_config.event[backend_config.events_active].inst = inst;
	backend_config.event[backend_config.events_active].channel.label = ident.label;
	backend_config.event[backend_config.events_active].value = val;
	backend_config.events_active++;
	LeaveCriticalSection(&backend_config.push_events);
	return 0;
}

//this state machine was copied more-or-less verbatim from the alsa midi implementation - fixes there will need to be integrated
static void winmidi_handle_epn(instance* inst, uint8_t chan, uint16_t control, uint16_t value){
	winmidi_instance_data* data = (winmidi_instance_data*) inst->impl;
	winmidi_channel_ident ident = {
		.label = 0
	};
	channel_value val;

	//switching between nrpn and rpn clears all valid bits
	if(((data->epn_status[chan] & EPN_NRPN) && (control == 101 || control == 100))
				|| (!(data->epn_status[chan] & EPN_NRPN) && (control == 99 || control == 98))){
		data->epn_status[chan] &= ~(EPN_NRPN | EPN_PARAMETER_LO | EPN_PARAMETER_HI);
	}

	//setting an address always invalidates the value valid bits
	if(control >= 98 && control <= 101){
		data->epn_status[chan] &= ~EPN_VALUE_HI;
	}

	//parameter hi
	if(control == 101 || control == 99){
		data->epn_control[chan] &= 0x7F;
		data->epn_control[chan] |= value << 7;
		data->epn_status[chan] |= EPN_PARAMETER_HI | ((control == 99) ? EPN_NRPN : 0);
		if(control == 101 && value == 127){
			data->epn_status[chan] &= ~EPN_PARAMETER_HI;
		}
	}

	//parameter lo
	if(control == 100 || control == 98){
		data->epn_control[chan] &= ~0x7F;
		data->epn_control[chan] |= value & 0x7F;
		data->epn_status[chan] |= EPN_PARAMETER_LO | ((control == 98) ? EPN_NRPN : 0);
		if(control == 100 && value == 127){
			data->epn_status[chan] &= ~EPN_PARAMETER_LO;
		}
	}

	//value hi, clears low, mark as update candidate
	if(control == 6
			//check if parameter is set before accepting value update
			&& ((data->epn_status[chan] & (EPN_PARAMETER_HI | EPN_PARAMETER_LO)) == (EPN_PARAMETER_HI | EPN_PARAMETER_LO))){
		data->epn_value[chan] = value << 7;
		data->epn_status[chan] |= EPN_VALUE_HI;
	}

	//value lo, flush the value
	if(control == 38
			&& data->epn_status[chan] & EPN_VALUE_HI){
		data->epn_value[chan] &= ~0x7F;
		data->epn_value[chan] |= value & 0x7F;
		data->epn_status[chan] &= ~EPN_VALUE_HI;

		//find the updated channel
		ident.fields.type = data->epn_status[chan] & EPN_NRPN ? nrpn : rpn;
		ident.fields.channel = chan;
		ident.fields.control = data->epn_control[chan];
		val.normalised = (double) data->epn_value[chan] / 16383.0;

		winmidi_enqueue_input(inst, ident,val);
	}
}

static void CALLBACK winmidi_input_callback(HMIDIIN device, unsigned message, DWORD_PTR inst, DWORD param1, DWORD param2){
	winmidi_channel_ident ident = {
		.label = 0
	};
	channel_value val = {
		0
	};
	union {
		struct {
			uint8_t status;
			uint8_t data1;
			uint8_t data2;
			uint8_t unused;
		} components;
		DWORD dword;
	} input = {
		.dword = 0
	};

	//callbacks may run on different threads, so we queue all events and alert the main thread via the feedback socket
	DBGPF("Input callback on thread %ld", GetCurrentThreadId());

	switch(message){
		case MIM_MOREDATA:
			//processing too slow, do not immediately alert the main loop
		case MIM_DATA:
			//param1 has the message
			input.dword = param1;
			ident.fields.channel = input.components.status & 0x0F;
			ident.fields.type = input.components.status & 0xF0;
			ident.fields.control = input.components.data1;
			val.normalised = (double) input.components.data2 / 127.0;
			val.raw.u64 = input.components.data2;

			if(ident.fields.type == 0x80){
				ident.fields.type = note;
				val.normalised = 0;
				val.raw.u64 = 0;
			}
			else if(ident.fields.type == pitchbend){
				ident.fields.control = 0;
				val.normalised = (double) ((input.components.data2 << 7) | input.components.data1) / 16383.0;
				val.raw.u64 = input.components.data2 << 7 | input.components.data1;
			}
			else if(ident.fields.type == aftertouch){
				ident.fields.control = 0;
				val.normalised = (double) input.components.data1 / 127.0;
				val.raw.u64 = input.components.data1;
			}
			break;
		case MIM_LONGDATA:
			//sysex message, ignore
			return;
		case MIM_ERROR:
			//error in input stream
			LOG("Error in input stream");
			return;
		case MIM_OPEN:
		case MIM_CLOSE:
			//device opened/closed
			return;
	}

	//pass changes in the (n)rpn CCs to the EPN state machine
	if(ident.fields.type == cc
			&& ((ident.fields.control <= 101 && ident.fields.control >= 98)
				|| ident.fields.control == 6
				|| ident.fields.control == 38)){
		winmidi_handle_epn((instance*) inst, ident.fields.channel, ident.fields.control, val.raw.u64);
	}

	DBGPF("Incoming message type %d channel %d control %d value %f",
			ident.fields.type, ident.fields.channel, ident.fields.control, val.normalised);
	if(winmidi_enqueue_input((instance*) inst, ident, val)){
		LOG("Failed to enqueue incoming data");
	}

	if(message != MIM_MOREDATA){
		//alert the main loop
		send(backend_config.socket_pair[1], "w", 1, 0);
	}
}

static void CALLBACK winmidi_output_callback(HMIDIOUT device, unsigned message, DWORD_PTR inst, DWORD param1, DWORD param2){
	DBGPF("Output callback on thread %ld", GetCurrentThreadId());
}

static int winmidi_match_input(char* prefix){
	MIDIINCAPS input_caps;
	unsigned inputs = midiInGetNumDevs();
	char* next_token = NULL;
	size_t n;

	if(!prefix){
		LOGPF("Detected %u input devices", inputs);
	}
	else{
		n = strtoul(prefix, &next_token, 10);
		if(!(*next_token) && n < inputs){
			midiInGetDevCaps(n, &input_caps, sizeof(MIDIINCAPS));
			LOGPF("Selected input device %s for ID %d", input_caps.szPname, n);
			return n;
		}
	}

	//find prefix match for input device
	for(n = 0; n < inputs; n++){
		midiInGetDevCaps(n, &input_caps, sizeof(MIDIINCAPS));
		if(!prefix){
			LOGPF("\tID %d: %s", n, input_caps.szPname);
		}
		else if(!strncmp(input_caps.szPname, prefix, strlen(prefix))){
			LOGPF("Selected input device %s (ID %" PRIsize_t ") for name %s", input_caps.szPname, n, prefix);
			return n;
		}
	}

	return -1;
}

static int winmidi_match_output(char* prefix){
	MIDIOUTCAPS output_caps;
	unsigned outputs = midiOutGetNumDevs();
	char* next_token = NULL;
	size_t n;

	if(!prefix){
		LOGPF("Detected %u output devices", outputs);
	}
	else{
		n = strtoul(prefix, &next_token, 10);
		if(!(*next_token) && n < outputs){
			midiOutGetDevCaps(n, &output_caps, sizeof(MIDIOUTCAPS));
			LOGPF("Selected output device %s for ID %d", output_caps.szPname, n);
			return n;
		}
	}

	//find prefix match for output device
	for(n = 0; n < outputs; n++){
		midiOutGetDevCaps(n, &output_caps, sizeof(MIDIOUTCAPS));
		if(!prefix){
			LOGPF("\tID %d: %s", n, output_caps.szPname);
		}
		else if(!strncmp(output_caps.szPname, prefix, strlen(prefix))){
			LOGPF("Selected output device %s (ID %" PRIsize_t " for name %s", output_caps.szPname, n, prefix);
			return n;
		}
	}

	return -1;
}

static int winmidi_socket_pair(int* fds){
	//this really should be a size_t but getsockname specifies int* for some reason
	int sockadd_len = sizeof(struct sockaddr_storage);
	char* error = NULL;
	struct sockaddr_storage sockadd = {
		0
	};

	//for some reason the feedback connection fails to work on 'real' windows with ipv6
	fds[0] = mmbackend_socket("127.0.0.1", "0", SOCK_DGRAM, 1, 0, 0);
	if(fds[0] < 0){
		LOG("Failed to open feedback socket");
		return 1;
	}

	if(getsockname(fds[0], (struct sockaddr*) &sockadd, &sockadd_len)){
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &error, 0, NULL);
		LOGPF("Failed to query feedback socket information: %s", error);
		LocalFree(error);
		return 1;
	}
	//getsockname on 'real' windows may not set the address - works on wine, though
	switch(sockadd.ss_family){
		case AF_INET:
		case AF_INET6:
			((struct sockaddr_in*) &sockadd)->sin_family = AF_INET;
			((struct sockaddr_in*) &sockadd)->sin_addr.s_addr = htobe32(INADDR_LOOPBACK);
			break;
		//for some absurd reason 'real' windows announces the socket as AF_INET6 but rejects any connection unless its AF_INET
//		case AF_INET6:
//			((struct sockaddr_in6*) &sockadd)->sin6_addr = in6addr_any;
//			break;
		default:
			LOG("Invalid feedback socket family");
			return 1;
	}
	DBGPF("Feedback socket family %d port %d", sockadd.ss_family, be16toh(((struct sockaddr_in*)&sockadd)->sin_port));
	fds[1] = socket(sockadd.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if(fds[1] < 0 || connect(backend_config.socket_pair[1], (struct sockaddr*) &sockadd, sockadd_len)){
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &error, 0, NULL);
		LOGPF("Failed to connect to feedback socket: %s", error);
		LocalFree(error);
		return 1;
	}

	return 0;
}

static int winmidi_start(size_t n, instance** inst){
	size_t p;
	int device, rv = -1;
	winmidi_instance_data* data = NULL;
	DBGPF("Main thread ID is %ld", GetCurrentThreadId());

	//output device list if requested
	if(backend_config.list_devices){
		winmidi_match_input(NULL);
		winmidi_match_output(NULL);
	}

	//open the feedback sockets
	if(winmidi_socket_pair(backend_config.socket_pair)){
		return 1;
	}

	//set up instances and start input
	for(p = 0; p < n; p++){
		data = (winmidi_instance_data*) inst[p]->impl;
		inst[p]->ident = p;

		//connect input device if requested
		if(data->read){
			device = winmidi_match_input(data->read);
			if(device < 0){
				LOGPF("Failed to match input device %s for instance %s", data->read, inst[p]->name);
				goto bail;
			}
			if(midiInOpen(&(data->device_in), device, (DWORD_PTR) winmidi_input_callback, (DWORD_PTR) inst[p], CALLBACK_FUNCTION | MIDI_IO_STATUS) != MMSYSERR_NOERROR){
				LOGPF("Failed to open input device for instance %s", inst[p]->name);
				goto bail;
			}
			//start midi input callbacks
			midiInStart(data->device_in);
		}

		//connect output device if requested
		if(data->write){
			device = winmidi_match_output(data->write);
			if(device < 0){
				LOGPF("Failed to match output device %s for instance %s", data->read, inst[p]->name);
				goto bail;
			}
			if(midiOutOpen(&(data->device_out), device, (DWORD_PTR) winmidi_output_callback, (DWORD_PTR) inst[p], CALLBACK_FUNCTION) != MMSYSERR_NOERROR){
				LOGPF("Failed to open output device for instance %s", inst[p]->name);
				goto bail;
			}
		}
	}

	//register the feedback socket to the core
	LOG("Registering 1 descriptor to core");
	if(mm_manage_fd(backend_config.socket_pair[0], BACKEND_NAME, 1, NULL)){
		goto bail;
	}

	rv = 0;
bail:
	return rv;
}

static int winmidi_shutdown(size_t n, instance** inst){
	size_t u;
	winmidi_instance_data* data = NULL;

	for(u = 0; u < n; u++){
		data = (winmidi_instance_data*) inst[u]->impl;
		free(data->read);
		data->read = NULL;
		free(data->write);
		data->write = NULL;

		if(data->device_in){
			midiInStop(data->device_in);
			midiInClose(data->device_in);
			data->device_in = NULL;
		}

		if(data->device_out){
			midiOutReset(data->device_out);
			midiOutClose(data->device_out);
			data->device_out = NULL;
		}

		free(inst[u]->impl);
	}

	closesocket(backend_config.socket_pair[0]);
	closesocket(backend_config.socket_pair[1]);

	EnterCriticalSection(&backend_config.push_events);
	free((void*) backend_config.event);
	backend_config.event = NULL;
	backend_config.events_alloc = 0;
	backend_config.events_active = 0;
	LeaveCriticalSection(&backend_config.push_events);
	DeleteCriticalSection(&backend_config.push_events);

	LOG("Backend shut down");
	return 0;
}
