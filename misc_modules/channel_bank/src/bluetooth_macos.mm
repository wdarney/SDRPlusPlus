#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include "bluetooth_macos.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>

static CBUUID* uuid(unsigned n) {
    return [CBUUID UUIDWithString:[NSString stringWithFormat:@"7d2f%04x-8c4b-4d7a-9a61-8e3c4f2a1000", n]];
}
static NSData* jsonData(id object) {
    return [NSJSONSerialization dataWithJSONObject:object options:0 error:nil] ?: [NSData data];
}
static NSDictionary* envelope(id identifier, NSInteger code, id body) {
    if (code >= 200 && code < 300)
        return @{@"v":@1, @"id":identifier ?: @0, @"ok":@YES, @"status":@(code), @"body":body ?: @{}};
    NSString* message = [body isKindOfClass:NSDictionary.class] ? body[@"error"] : nil;
    return @{@"v":@1, @"id":identifier ?: @0, @"ok":@NO, @"status":@(code),
             @"error":@{@"code":@"request_failed", @"message":[message isKindOfClass:NSString.class] ? message : @"Request unavailable"}};
}

@interface CBMacMessage : NSObject
@property CBCentral* central;
@property CBMutableCharacteristic* characteristic;
@property NSData* payload;
@property NSUInteger offset;
@property uint16_t identifier;
@end
@implementation CBMacMessage
@end

@interface CBMacServer : NSObject <CBPeripheralManagerDelegate> {
@public
    dispatch_queue_t queue;
    std::thread worker;
    std::atomic<bool> stopping;
}
@property CBPeripheralManager* manager;
@property NSMutableArray<CBMutableCharacteristic*>* characteristics;
@property NSMutableDictionary<NSUUID*, CBCentral*>* centrals;
@property NSMutableDictionary<NSString*, NSMutableData*>* assemblies;
@property NSMutableDictionary<NSString*, NSDate*>* assemblyTimes;
@property NSMutableArray<NSDictionary*>* commands;
@property NSMutableArray<CBMacMessage*>* outgoing;
@property NSMutableDictionary<NSString*, NSData*>* readCache;
@property NSData* fullState;
@property NSData* summary;
@property NSString* baseURL;
@property NSString* statusText;
@property NSUInteger tick;
@property uint64_t sequence;
- (void)pump;
- (void)work;
@end

@implementation CBMacServer
- (instancetype)init {
    if ((self = [super init])) {
        queue = dispatch_queue_create("sdrpp.channel-bank.bluetooth", DISPATCH_QUEUE_SERIAL);
        stopping = false;
        _centrals = [NSMutableDictionary new];
        _assemblies = [NSMutableDictionary new];
        _assemblyTimes = [NSMutableDictionary new];
        _commands = [NSMutableArray new];
        _outgoing = [NSMutableArray new];
        _readCache = [NSMutableDictionary new];
        _fullState = jsonData(envelope(@0, 503, @{@"error":@"Waiting for Channel Bank"}));
        _summary = _fullState;
        _statusText = @"Waiting for Bluetooth";
    }
    return self;
}
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
    if (stopping) return;
    [_outgoing removeAllObjects];
    [_centrals removeAllObjects];
    [_assemblies removeAllObjects];
    [_assemblyTimes removeAllObjects];
    [_commands removeAllObjects];
    [_readCache removeAllObjects];
    if (peripheral.state != CBManagerStatePoweredOn) {
        _statusText = peripheral.state == CBManagerStateUnauthorized ? @"Bluetooth permission denied"
            : peripheral.state == CBManagerStateUnsupported ? @"Bluetooth unsupported"
            : @"Bluetooth is off or unavailable";
        return;
    }
    [peripheral removeAllServices];
    _characteristics = [NSMutableArray new];
    for (unsigned n = 1; n <= 6; ++n) {
        CBCharacteristicProperties properties = CBCharacteristicPropertyRead;
        CBAttributePermissions permissions = CBAttributePermissionsReadable;
        if (n == 2) {
            properties = CBCharacteristicPropertyWrite;
            permissions = CBAttributePermissionsWriteable;
        } else if (n >= 3) {
            properties |= n == 5 ? CBCharacteristicPropertyNotify : CBCharacteristicPropertyIndicate;
        }
        [_characteristics addObject:[[CBMutableCharacteristic alloc] initWithType:uuid(n)
            properties:properties value:nil permissions:permissions]];
    }
    CBMutableService* service = [[CBMutableService alloc] initWithType:uuid(0) primary:YES];
    service.characteristics = _characteristics;
    [peripheral addService:service];
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didAddService:(CBService*)service error:(NSError*)error {
    if (stopping) return;
    if (error) { _statusText = error.localizedDescription; return; }
    [peripheral startAdvertising:@{CBAdvertisementDataServiceUUIDsKey:@[uuid(0)],
                                   CBAdvertisementDataLocalNameKey:@"SDR++ Channel Bank"}];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)peripheral error:(NSError*)error {
    _statusText = error ? error.localizedDescription : @"Bluetooth ready for iPhone";
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral central:(CBCentral*)central
        didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
    if (_centrals.count >= 4 && !_centrals[central.identifier]) return;
    _centrals[central.identifier] = central;
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral central:(CBCentral*)central
        didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
    NSIndexSet* remove = [_outgoing indexesOfObjectsPassingTest:^BOOL(CBMacMessage* item, NSUInteger idx, BOOL* stop) {
        return [item.central.identifier isEqual:central.identifier] && [item.characteristic.UUID isEqual:characteristic.UUID];
    }];
    [_outgoing removeObjectsAtIndexes:remove];
    BOOL subscribed = NO;
    for (CBMutableCharacteristic* c in _characteristics)
        for (CBCentral* subscriber in c.subscribedCentrals)
            if ([subscriber.identifier isEqual:central.identifier]) subscribed = YES;
    if (!subscribed) {
        [_centrals removeObjectForKey:central.identifier];
        NSString* prefix = central.identifier.UUIDString;
        for (NSString* key in [_readCache.allKeys copy])
            if ([key hasPrefix:prefix]) [_readCache removeObjectForKey:key];
        [_assemblies removeObjectForKey:prefix];
        [_assemblyTimes removeObjectForKey:prefix];
    }
}
- (void)enqueue:(NSData*)data characteristic:(CBMutableCharacteristic*)characteristic
        central:(CBCentral*)central identifier:(uint16_t)identifier {
    if (data.length > 256 * 1024) return;
    BOOL subscribed = NO;
    for (CBCentral* c in characteristic.subscribedCentrals)
        if ([c.identifier isEqual:central.identifier]) subscribed = YES;
    if (!subscribed) return;
    // Finish messages already started; coalesce snapshots to avoid BLE backlogs.
    if (![characteristic.UUID isEqual:uuid(3)]) {
        for (CBMacMessage* item in _outgoing)
            if ([item.central.identifier isEqual:central.identifier] && item.characteristic == characteristic) return;
    }
    if (_outgoing.count >= 32) return;
    CBMacMessage* message = [CBMacMessage new];
    message.payload = data;
    message.central = central;
    message.characteristic = characteristic;
    message.identifier = identifier;
    [_outgoing addObject:message];
    [self pump];
}
- (void)pump {
    while (_outgoing.count && !stopping) {
        CBMacMessage* message = _outgoing.firstObject;
        NSUInteger mtu = std::min<NSUInteger>(512, message.central.maximumUpdateValueLength);
        if (mtu <= 8) { [_outgoing removeObjectAtIndex:0]; continue; }
        NSUInteger count = std::min(mtu - 8, message.payload.length - message.offset);
        uint32_t offset = (uint32_t)message.offset;
        uint8_t flags = (offset == 0 ? 1 : 0) | (offset + count == message.payload.length ? 2 : 0);
        uint8_t header[8] = {1, flags, (uint8_t)message.identifier, (uint8_t)(message.identifier >> 8),
            (uint8_t)offset, (uint8_t)(offset >> 8), (uint8_t)(offset >> 16), (uint8_t)(offset >> 24)};
        NSMutableData* frame = [NSMutableData dataWithBytes:header length:8];
        [frame appendData:[message.payload subdataWithRange:NSMakeRange(offset, count)]];
        if (![_manager updateValue:frame forCharacteristic:message.characteristic onSubscribedCentrals:@[message.central]]) return;
        message.offset += count;
        if (flags & 2) [_outgoing removeObjectAtIndex:0];
    }
}
- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)peripheral { [self pump]; }
- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveReadRequest:(CBATTRequest*)request {
    NSString* key = [NSString stringWithFormat:@"%@/%@", request.central.identifier, request.characteristic.UUID];
    if (request.offset == 0) {
        NSData* value = [NSData data];
        if ([request.characteristic.UUID isEqual:uuid(1)])
            value = jsonData(@{@"protocol":@"sdrpp.channel-bank.gatt", @"version":@1,
                @"encoding":@"json-utf8", @"maxAttributeValueBytes":@512, @"maxRequestBytes":@65536,
                @"frameHeader":@"u8 version,u8 flags,u16le messageId,u32le offset",
                @"summaryCharacteristic":uuid(6).UUIDString,
                @"audio":@{@"available":@NO, @"format":@"pcm_s16le", @"rate":@48000, @"channels":@1}});
        else if ([request.characteristic.UUID isEqual:uuid(4)]) value = _fullState;
        else if ([request.characteristic.UUID isEqual:uuid(6)]) value = _summary;
        else if ([request.characteristic.UUID isEqual:uuid(3)]) value = _readCache[key] ?: value;
        if (_readCache.count < 32 || _readCache[key]) _readCache[key] = value;
    }
    NSData* value = _readCache[key] ?: [NSData data];
    if (request.offset > value.length) { [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset]; return; }
    request.value = [value subdataWithRange:NSMakeRange(request.offset, value.length - request.offset)];
    [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
}
- (void)peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
    // The protocol fragments at the application layer and does not use prepared writes.
    if (requests.count != 1) {
        if (requests.count) [peripheral respondToRequest:requests.firstObject withResult:CBATTErrorRequestNotSupported];
        return;
    }
    for (CBATTRequest* request in requests) {
        NSData* data = request.value;
        if (!_centrals[request.central.identifier]) {
            [peripheral respondToRequest:request withResult:CBATTErrorInsufficientAuthorization]; continue;
        }
        if (stopping || ![request.characteristic.UUID isEqual:uuid(2)] || request.offset || data.length < 8) {
            [peripheral respondToRequest:request withResult:CBATTErrorInvalidAttributeValueLength]; continue;
        }
        const uint8_t* p = (const uint8_t*)data.bytes;
        uint16_t identifier = p[2] | (p[3] << 8);
        uint32_t offset = p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        NSString* key = request.central.identifier.UUIDString;
        if (p[0] != 1 || (p[1] & ~3) || (p[1] & 1 && offset) || _commands.count >= 16 || _outgoing.count >= 24) {
            [peripheral respondToRequest:request withResult:CBATTErrorInsufficientResources]; continue;
        }
        if (p[1] & 1) {
            if (_assemblies.count >= 4 && !_assemblies[key]) {
                [peripheral respondToRequest:request withResult:CBATTErrorInsufficientResources]; continue;
            }
            _assemblies[key] = [NSMutableData dataWithBytes:p+2 length:2];
            _assemblyTimes[key] = [NSDate date];
        }
        NSMutableData* assembly = _assemblies[key];
        if (!assembly || assembly.length != offset + 2 || memcmp(assembly.bytes, p+2, 2) ||
            offset + data.length - 8 > 65536) {
            [_assemblies removeObjectForKey:key];
            [_assemblyTimes removeObjectForKey:key];
            [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset]; continue;
        }
        [assembly appendBytes:p+8 length:data.length-8];
        if (p[1] & 2) {
            NSData* payload = [assembly subdataWithRange:NSMakeRange(2, assembly.length-2)];
            [_commands addObject:@{@"central":request.central, @"id":@(identifier), @"payload":payload}];
            [_assemblies removeObjectForKey:key];
            [_assemblyTimes removeObjectForKey:key];
        }
        [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
    }
}
- (NSDictionary*)perform:(NSDictionary*)command {
    if (![command isKindOfClass:NSDictionary.class]) return envelope(@0, 400, nil);
    id identifier = [command[@"id"] isKindOfClass:NSNumber.class] ? command[@"id"] : @0;
    NSString* method = command[@"method"];
    NSString* path = command[@"path"];
    NSArray* gets = @[@"/api/state", @"/state", @"/api/state/summary", @"/api/sources",
        @"/api/source-controls", @"/api/source-offset", @"/api/sdrpp-server", @"/api/channel-bank/settings", @"/api/recordings"];
    NSArray* posts = @[@"/api/start", @"/api/stop", @"/api/play", @"/api/stop-radio", @"/api/radio/stop",
        @"/api/center", @"/api/source", @"/api/source-controls", @"/api/source-offset",
        @"/api/sdrpp-server", @"/api/sdrpp-server/connect", @"/api/sdrpp-server/disconnect",
        @"/api/channel-bank/settings", @"/api/frequency/block", @"/api/playback-lock",
        @"/api/recordings/session", @"/api/recordings/clear-wavs"];
    if (![command[@"v"] isEqual:@1] || ![path isKindOfClass:NSString.class] ||
        !(([method isEqual:@"GET"] && [gets containsObject:path]) || ([method isEqual:@"POST"] && [posts containsObject:path])))
        return envelope(identifier, 400, @{@"error":@"Unsupported Bluetooth request (audio and file downloads are not available)"});
    BOOL summary = [path isEqual:@"/api/state/summary"];
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:
        [NSURL URLWithString:[_baseURL stringByAppendingString:summary ? @"/api/state" : path]]
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:7];
    request.HTTPMethod = method;
    if ([method isEqual:@"POST"]) {
        if (command[@"body"] && ![command[@"body"] isKindOfClass:NSDictionary.class]) return envelope(identifier, 400, nil);
        request.HTTPBody = jsonData(command[@"body"] ?: @{});
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    }
    NSURLResponse* response = nil;
    NSError* error = nil;
    // This runs only on our joined worker, never CoreBluetooth's delegate queue or SDR++'s UI.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSData* data = [NSURLConnection sendSynchronousRequest:request returningResponse:&response error:&error];
#pragma clang diagnostic pop
    id body = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSInteger code = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse*)response).statusCode : 503;
    if (error || ![body isKindOfClass:NSDictionary.class]) return envelope(identifier, 503, @{@"error":@"Channel Bank Web Control unavailable"});
    if (code == 200 && ([path isEqual:@"/api/state"] || summary || [method isEqual:@"POST"])) {
        NSMutableDictionary* state = [body mutableCopy];
        state[@"seq"] = @(++_sequence);
        body = state;
    }
    if (summary && code == 200) {
        NSMutableDictionary* compact = [NSMutableDictionary new];
        for (NSString* key in @[@"seq", @"serverTimeMs", @"running", @"radioPlaying", @"selectedSource",
                @"centerHz", @"sampleRate", @"mode", @"demodMode", @"snrThresholdDb", @"maxChannels",
                @"recordingEnabled", @"playbackQueued", @"playback"])
            if (body[key]) compact[key] = body[key];
        compact[@"activeChannelCount"] = @([body[@"activeChannels"] count]);
        body = compact;
    }
    return envelope(identifier, code, body);
}
- (void)work {
    while (!stopping) {
        @autoreleasepool {
            __block NSDictionary* command = nil;
            __block BOOL subscribed = NO;
            dispatch_sync(queue, ^{
                if (self.commands.count) { command = self.commands.firstObject; [self.commands removeObjectAtIndex:0]; }
                subscribed = self.centrals.count > 0;
                for (NSString* key in [self.assemblyTimes.allKeys copy])
                    if (-[self.assemblyTimes[key] timeIntervalSinceNow] > 10) {
                        [self.assemblies removeObjectForKey:key]; [self.assemblyTimes removeObjectForKey:key];
                    }
            });
            if (command) {
                NSDictionary* input = [NSJSONSerialization JSONObjectWithData:command[@"payload"] options:0 error:nil];
                NSData* result = jsonData([self perform:input]);
                dispatch_sync(queue, ^{
                    if (self->stopping) return;
                    CBCentral* central = command[@"central"];
                    self.readCache[[NSString stringWithFormat:@"%@/%@", central.identifier, uuid(3)]] = result;
                    [self enqueue:result characteristic:self.characteristics[2] central:central identifier:[command[@"id"] unsignedShortValue]];
                });
            }
            if (subscribed && !stopping) {
                BOOL full = (_tick++ % 10) == 0;
                NSData* result = jsonData([self perform:@{@"v":@1, @"id":@0, @"method":@"GET",
                    @"path":full ? @"/api/state" : @"/api/state/summary"}]);
                dispatch_sync(queue, ^{
                    if (self->stopping || self.characteristics.count != 6) return;
                    if (full) self.fullState = result; else self.summary = result;
                    for (CBCentral* central in self.centrals.allValues)
                        [self enqueue:result characteristic:self.characteristics[full ? 3 : 5] central:central identifier:0];
                });
            }
        }
        for (int i = 0; i < 10 && !stopping; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
@end

namespace channel_bank_bluetooth {
void* start(const std::string& host, int port) {
    CBMacServer* server = [CBMacServer new];
    server.baseURL = [NSString stringWithFormat:@"http://%s:%d", host.c_str(), port];
    if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"]) {
        server.statusText = @"Rebuild app with Bluetooth permission description";
    } else {
        dispatch_sync(server->queue, ^{
            server.manager = [[CBPeripheralManager alloc] initWithDelegate:server queue:server->queue options:nil];
        });
        server->worker = std::thread([server] { [server work]; });
    }
    return (__bridge_retained void*)server;
}
void stop(void* handle) {
    if (!handle) return;
    CBMacServer* server = (__bridge_transfer CBMacServer*)handle;
    server->stopping = true;
    if (server->worker.joinable()) server->worker.join();
    dispatch_sync(server->queue, ^{
        server.manager.delegate = nil;
        [server.manager stopAdvertising];
        [server.manager removeAllServices];
        server.manager = nil;
        [server.outgoing removeAllObjects];
        [server.commands removeAllObjects];
    });
}
std::string status(void* handle) {
    if (!handle) return "Bluetooth stopped";
    CBMacServer* server = (__bridge CBMacServer*)handle;
    __block NSString* status;
    dispatch_sync(server->queue, ^{ status = [NSString stringWithFormat:@"%@ (%lu clients)",
        server.statusText, (unsigned long)server.centrals.count]; });
    return status.UTF8String ?: "Bluetooth unavailable";
}
}
