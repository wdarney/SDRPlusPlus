// Radio-free delegate tests: no CBPeripheralManager is created.
#include "../src/bluetooth_macos.mm"
#include <cassert>
#include <iostream>
#include <fcntl.h>

@interface TestCentral : NSObject
@property NSUUID* identifier;
@property NSUInteger maximumUpdateValueLength;
@end
@implementation TestCentral
@end
@interface TestRequest : NSObject
@property CBCentral* central;
@property CBCharacteristic* characteristic;
@property NSData* value;
@property NSUInteger offset;
@end
@implementation TestRequest
@end
@interface TestCharacteristic : CBMutableCharacteristic
@property NSArray<CBCentral*>* testSubscribers;
@end
@implementation TestCharacteristic
- (NSArray<CBCentral*>*)subscribedCentrals { return _testSubscribers ?: @[]; }
@end
@interface TestPeripheral : NSObject
@property CBATTError lastResult;
@property BOOL writable;
@property NSMutableArray<NSData*>* frames;
@property NSMutableArray<CBUUID*>* types;
@end
@implementation TestPeripheral
- (void)respondToRequest:(CBATTRequest*)request withResult:(CBATTError)result { _lastResult = result; }
- (BOOL)updateValue:(NSData*)value forCharacteristic:(CBMutableCharacteristic*)characteristic onSubscribedCentrals:(NSArray*)centrals {
    if (!_writable) return NO;
    [_frames addObject:value];
    if (characteristic.UUID) [_types addObject:characteristic.UUID];
    return YES;
}
@end

static NSData* frame(uint8_t flags, uint16_t identifier, uint32_t offset, NSData* payload) {
    uint8_t header[] = {1, flags, (uint8_t)identifier, (uint8_t)(identifier>>8),
        (uint8_t)offset, (uint8_t)(offset>>8), (uint8_t)(offset>>16), (uint8_t)(offset>>24)};
    NSMutableData* result = [NSMutableData dataWithBytes:header length:8];
    [result appendData:payload];
    return result;
}

static void testBinaryWindows(CBMacServer* server, TestCentral* central, TestPeripheral* peripheral) {
    server.characteristics = [NSMutableArray new];
    for (unsigned n = 1; n <= 8; ++n) {
        TestCharacteristic* c = [[TestCharacteristic alloc] initWithType:uuid(n)
            properties:CBCharacteristicPropertyNotify value:nil permissions:CBAttributePermissionsReadable];
        c.testSubscribers = @[(CBCentral*)central];
        [server.characteristics addObject:c];
    }
    char path[] = "/tmp/cb-binary-audio-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && unlink(path) == 0);
    NSMutableData* original = [NSMutableData dataWithLength:21000];
    for (NSUInteger i = 0; i < original.length; ++i) ((uint8_t*)original.mutableBytes)[i] = i % 251;
    assert(write(fd, original.bytes, original.length) == (ssize_t)original.length);
    server->openPlayback = [fd](std::string& name) { name = "voice.m4a"; return dup(fd); };
    auto request = [&](NSString* route, NSDictionary* body, NSUUID* owner) {
        return [server perform:@{@"v":@1, @"id":@44, @"method":@"GET", @"path":route, @"body":body} owner:owner];
    };
    NSString* route = @"/api/audio/window";
    NSDictionary* ready = request(@"/api/audio/current-playback", @{@"transport":@"binary-v1"}, central.identifier)[@"body"];
    NSString* token = ready[@"transferId"];
    assert(token.length && [ready[@"size"] intValue] == 21000 && !ready[@"dataBase64"]);
    assert([ready[@"streamId"] unsignedIntValue] != 0);
    close(fd);
    server->openPlayback = [](std::string&) { return -1; };
    assert([request(route, @{@"transferId":token, @"offset":@0, @"windowId":@1}, NSUUID.UUID)[@"status"] intValue] == 404);
    assert([request(route, @{@"transferId":token, @"offset":@-1, @"windowId":@1}, central.identifier)[@"status"] intValue] == 400);
    assert([request(route, @{@"transferId":token, @"offset":@0, @"windowId":@0}, central.identifier)[@"status"] intValue] == 416);
    TestCharacteristic* audio = (TestCharacteristic*)server.characteristics[7];
    audio.testSubscribers = @[];
    assert([request(route, @{@"transferId":token, @"offset":@0, @"windowId":@1}, central.identifier)[@"status"] intValue] == 409);
    audio.testSubscribers = @[(CBCentral*)central];
    for (NSNumber* mtu in @[@20, @185, @512]) {
        central.maximumUpdateValueLength = mtu.unsignedIntegerValue;
        peripheral.writable = NO;
        [peripheral.frames removeAllObjects];
        [server.outgoing removeAllObjects];
        NSDictionary* descriptor = request(route, @{@"transferId":token, @"offset":@0, @"windowId":@1}, central.identifier)[@"body"];
        assert([descriptor[@"chunkBytes"] unsignedIntegerValue] == mtu.unsignedIntegerValue - 16);
        [server pump];
        assert(server.outgoing.count == 1 && server.outgoing.firstObject.offset == 0);
        // Replace a stalled window, then serve a command before any audio bytes.
        descriptor = request(route, @{@"transferId":token, @"offset":@0, @"windowId":@2}, central.identifier)[@"body"];
        assert(server.outgoing.count == 1);
        [server enqueue:jsonData(@{@"ok":@YES}) characteristic:server.characteristics[2]
            central:(CBCentral*)central identifier:44];
        assert([server.outgoing.firstObject.characteristic.UUID isEqual:uuid(3)]);
        [server.outgoing removeObjectAtIndex:0];
        peripheral.writable = YES;
        [server pump];
        assert(peripheral.frames.count == 16 && server.outgoing.count == 0);
        NSMutableData* received = [NSMutableData new];
        for (NSData* packet in peripheral.frames) {
            assert(packet.length <= mtu.unsignedIntegerValue);
            const uint8_t* p = (const uint8_t*)packet.bytes;
            auto u32 = [&](unsigned at) { return p[at] | uint32_t(p[at+1]) << 8 | uint32_t(p[at+2]) << 16 | uint32_t(p[at+3]) << 24; };
            assert(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 0);
            assert(u32(4) == [ready[@"streamId"] unsignedIntValue] && u32(8) == 2);
            assert(u32(12) == received.length);
            [received appendData:[packet subdataWithRange:NSMakeRange(16, packet.length - 16)]];
        }
        assert([received isEqual:[original subdataWithRange:NSMakeRange(0, received.length)]]);
        assert(received.length == [descriptor[@"nextOffset"] unsignedIntegerValue]);
    }
    // Every window, including the short EOF window, retains its lease for retry.
    NSMutableData* received = [NSMutableData new];
    uint32_t window = 3;
    while (received.length < original.length) {
        [peripheral.frames removeAllObjects];
        NSDictionary* descriptor = request(route, @{@"transferId":token, @"offset":@(received.length), @"windowId":@(window++)}, central.identifier)[@"body"];
        assert(descriptor);
        [server pump];
        for (NSData* packet in peripheral.frames) [received appendData:[packet subdataWithRange:NSMakeRange(16, packet.length - 16)]];
        assert(received.length == [descriptor[@"nextOffset"] unsignedIntegerValue]);
    }
    assert([received isEqual:original] && server.transfers[token]);
    request(route, @{@"transferId":token, @"offset":@0, @"windowId":@(window++)}, central.identifier);
    assert(server.outgoing.count == 1);
    NSDictionary* fallback = request(@"/api/audio/current-playback", @{@"transferId":token, @"offset":@0, @"limit":@4096}, central.identifier)[@"body"];
    NSData* fallbackBytes = [[NSData alloc] initWithBase64EncodedString:fallback[@"dataBase64"] options:0];
    assert([fallbackBytes isEqual:[original subdataWithRange:NSMakeRange(0, 4096)]]);
    assert(server.outgoing.count == 0 && server.transfers[token]);
    request(route, @{@"transferId":token, @"offset":@0, @"windowId":@(window++)}, central.identifier);
    assert([request(@"/api/audio/current-playback", @{@"transferId":token, @"cancel":@YES}, central.identifier)[@"status"] intValue] == 200);
    assert(server.transfers.count == 0 && server.outgoing.count == 0);
    assert([request(route, @{@"transferId":token, @"offset":@0, @"windowId":@(window)}, central.identifier)[@"status"] intValue] == 404);
    std::cout << "Binary windows: subscription, ownership, retries, MTU 20/185/512, command priority and EOF cleanup passed\n";
}
static void testCompressedPlayback(CBMacServer* server, NSUUID* owner) {
    // Ten seconds of 48 kHz mono PCM, independent of the local recording pipeline.
    NSMutableData* wav = [NSMutableData new];
    auto bytes = [&](const char* text) { [wav appendBytes:text length:4]; };
    auto u16 = [&](uint16_t n) { uint8_t b[] = {(uint8_t)n, (uint8_t)(n >> 8)}; [wav appendBytes:b length:2]; };
    auto u32 = [&](uint32_t n) { uint8_t b[] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)}; [wav appendBytes:b length:4]; };
    bytes("RIFF"); u32(36 + 480000 * 2); bytes("WAVE"); bytes("fmt "); u32(16);
    u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16); bytes("data"); u32(480000 * 2);
    for (int i = 0; i < 480000; ++i) u16(static_cast<int16_t>(12000 * std::sin(i * 2 * 3.141592653589793 * 440 / 48000)));
    char sourcePath[] = "/tmp/cb-ble-aac-source-XXXXXX";
    int source = mkstemp(sourcePath);
    assert(source >= 0 && write(source, wav.bytes, wav.length) == (ssize_t)wav.length);
    server->openPlayback = [source](std::string& name) { name = "voice.wav"; return dup(source); };
    auto page = [&](NSDictionary* body) {
        return [server perform:@{@"v":@1, @"id":@42, @"method":@"GET",
            @"path":@"/api/audio/current-playback", @"body":body} owner:owner];
    };
    NSDictionary* response = page(@{@"encoding":@"aac", @"offset":@0});
    assert([response[@"status"] intValue] == 202 || [response[@"status"] intValue] == 200);
    NSString* token = response[@"body"][@"transferId"];
    assert(token.length);
    NSMutableData* unchanged = [NSMutableData dataWithLength:wav.length];
    assert(pread(source, unchanged.mutableBytes, unchanged.length, 0) == (ssize_t)unchanged.length);
    assert([unchanged isEqual:wav]);
    assert(unlink(sourcePath) == 0);
    close(source);
    server->openPlayback = [](std::string&) { return -1; };
    NSMutableData* compressed = [NSMutableData new];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (true) {
        assert(std::chrono::steady_clock::now() < deadline);
        NSDictionary* body = response[@"body"];
        if (!body) std::cerr << response.description.UTF8String << '\n';
        assert([body[@"transferId"] isEqual:token]);
        assert([body[@"name"] isEqual:@"voice.m4a"] && [body[@"contentType"] isEqual:@"audio/mp4"]);
        if ([body[@"preparing"] boolValue]) {
            assert([response[@"status"] intValue] == 202 && !body[@"dataBase64"]);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            assert([response[@"status"] intValue] == 200);
            assert([body[@"offset"] unsignedLongLongValue] == compressed.length);
            [compressed appendData:[[NSData alloc] initWithBase64EncodedString:body[@"dataBase64"] options:0]];
            if ([body[@"eof"] boolValue]) { assert(compressed.length == [body[@"size"] unsignedLongLongValue]); break; }
        }
        response = page(@{@"transferId":token, @"offset":@(compressed.length), @"limit":@4096});
    }
    assert(server.transfers.count == 0 && compressed.length < wav.length / 5);
    char outputPath[] = "/tmp/cb-ble-aac-decode-XXXXXX";
    int output = mkstemp(outputPath);
    assert(output >= 0 && write(output, compressed.bytes, compressed.length) == (ssize_t)compressed.length);
    close(output);
    NSURL* url = [NSURL fileURLWithFileSystemRepresentation:outputPath isDirectory:NO relativeToURL:nil];
    ExtAudioFileRef decoded = nullptr;
    assert(ExtAudioFileOpenURL((__bridge CFURLRef)url, &decoded) == noErr);
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    assert(ExtAudioFileGetProperty(decoded, kExtAudioFileProperty_FileDataFormat, &size, &format) == noErr);
    assert(format.mFormatID == kAudioFormatMPEG4AAC && format.mChannelsPerFrame == 1 && format.mSampleRate == 24000);
    format = {24000, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked, 4, 1, 4, 1, 32, 0};
    assert(ExtAudioFileSetProperty(decoded, kExtAudioFileProperty_ClientDataFormat, sizeof(format), &format) == noErr);
    uint64_t totalFrames = 0;
    double energy = 0;
    while (true) {
        float samples[4096];
        AudioBufferList buffer{1, {{1, sizeof(samples), samples}}};
        UInt32 count = 4096;
        assert(ExtAudioFileRead(decoded, &count, &buffer) == noErr);
        if (!count) break;
        totalFrames += count;
        for (UInt32 i = 0; i < count; ++i) energy += samples[i] * samples[i];
    }
    assert(totalFrames >= 239000 && totalFrames < 243000 && energy / totalFrames > 0.01);
    ExtAudioFileDispose(decoded);
    unlink(outputPath);
    std::cout << "AAC fixture: " << wav.length << " WAV bytes -> " << compressed.length
              << " M4A bytes; decoded " << totalFrames << " frames\n";

    char badPath[] = "/tmp/cb-ble-aac-invalid-XXXXXX";
    int bad = mkstemp(badPath);
    assert(bad >= 0 && write(bad, "invalid", 7) == 7 && unlink(badPath) == 0);
    server->openPlayback = [bad](std::string& name) { name = "bad.wav"; return dup(bad); };
    response = page(@{@"encoding":@"aac"});
    token = response[@"body"][@"transferId"];
    while ([response[@"status"] intValue] == 202) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        response = page(@{@"transferId":token});
    }
    assert([response[@"status"] intValue] == 502 && server.transfers.count == 0);
    response = page(@{@"encoding":@"aac"});
    if ([response[@"status"] intValue] == 202) {
        token = response[@"body"][@"transferId"];
        assert([page(@{@"transferId":token, @"cancel":@YES})[@"status"] intValue] == 200);
    }
    assert(server.transfers.count == 0);
    close(bad);
    server->openPlayback = [](std::string&) { return -1; };
}

int main() {
    @autoreleasepool {
        CBMacServer* server = [CBMacServer new];
        TestPeripheral* peripheral = [TestPeripheral new];
        peripheral.frames = [NSMutableArray new];
        server.manager = (CBPeripheralManager*)peripheral;
        TestCentral* central = [TestCentral new];
        central.identifier = [NSUUID UUID];
        central.maximumUpdateValueLength = 20;
        server.centrals[central.identifier] = (CBCentral*)central;
        TestRequest* request = [TestRequest new];
        request.central = (CBCentral*)central;
        request.characteristic = [[CBMutableCharacteristic alloc] initWithType:uuid(2)
            properties:CBCharacteristicPropertyWrite value:nil permissions:CBAttributePermissionsWriteable];
        NSData* payload = jsonData(@{@"v":@1, @"id":@513, @"method":@"POST", @"path":@"/api/center", @"body":@{@"hz":@132000000}});
        request.value = frame(1, 513, 0, [payload subdataWithRange:NSMakeRange(0, 10)]);
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:@[(CBATTRequest*)request]];
        assert(peripheral.lastResult == CBATTErrorSuccess && server.commands.count == 0);
        request.value = frame(2, 513, 10, [payload subdataWithRange:NSMakeRange(10, payload.length-10)]);
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:@[(CBATTRequest*)request]];
        assert(server.commands.count == 1);
        assert([server.commands[0][@"payload"] isEqual:payload]);
        [server.commands removeAllObjects];
        request.value = frame(2, 513, 50, payload);
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:@[(CBATTRequest*)request]];
        assert(peripheral.lastResult == CBATTErrorInvalidOffset && server.commands.count == 0);
        request.value = frame(3, 1, 0, [NSMutableData dataWithLength:65537]);
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveWriteRequests:@[(CBATTRequest*)request]];
        assert(peripheral.lastResult != CBATTErrorSuccess && server.commands.count == 0);
        // Backpressure must retain the exact next fragment, then resume at its offset.
        CBMacMessage* message = [CBMacMessage new];
        message.central = (CBCentral*)central;
        message.payload = payload;
        message.identifier = 513;
        [server.outgoing addObject:message];
        [server pump];
        assert(message.offset == 0 && peripheral.frames.count == 0);
        peripheral.writable = YES;
        [server pump];
        assert(server.outgoing.count == 0 && peripheral.frames.count > 1);
        NSMutableData* assembled = [NSMutableData new];
        for (NSData* part in peripheral.frames) {
            assert(part.length <= 20);
            const uint8_t* p = (const uint8_t*)part.bytes;
            assert(p[0] == 1 && p[2] == 1 && p[3] == 2);
            uint32_t offset = p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
            assert(offset == assembled.length);
            [assembled appendData:[part subdataWithRange:NSMakeRange(8, part.length-8)]];
        }
        assert([assembled isEqual:payload]);
        assert(((const uint8_t*)peripheral.frames.firstObject.bytes)[1] & 1);
        assert(((const uint8_t*)peripheral.frames.lastObject.bytes)[1] & 2);
        NSDictionary* rejected = [server perform:@{@"v":@1, @"id":@4, @"method":@"GET", @"path":@"http://example.invalid"}];
        assert([rejected[@"status"] intValue] == 400);
        assert([rejected[@"id"] intValue] == 4);
        // Long reads retain a snapshot across offsets.
        request.characteristic = [[CBMutableCharacteristic alloc] initWithType:uuid(4)
            properties:CBCharacteristicPropertyRead value:nil permissions:CBAttributePermissionsReadable];
        server.fullState = payload;
        request.offset = 0;
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveReadRequest:(CBATTRequest*)request];
        server.fullState = [NSData data];
        request.offset = 10;
        [server peripheralManager:(CBPeripheralManager*)peripheral didReceiveReadRequest:(CBATTRequest*)request];
        assert([request.value isEqual:[payload subdataWithRange:NSMakeRange(10, payload.length-10)]]);
        // Hold the exact file across playback advancing, unlink, and multiple BLE pages.
        char path[] = "/tmp/channel-bank-ble-XXXXXX";
        int file = mkstemp(path);
        assert(file >= 0);
        NSMutableData* recording = [NSMutableData dataWithLength:40000];
        for (NSUInteger i = 0; i < recording.length; ++i) ((uint8_t*)recording.mutableBytes)[i] = i % 251;
        assert(write(file, recording.bytes, recording.length) == (ssize_t)recording.length);
        server->openPlayback = [file](std::string& name) { name = "transmission.wav"; return dup(file); };
        auto page = [&](NSDictionary* body, NSUUID* owner) {
            return [server perform:@{@"v":@1, @"id":@42, @"method":@"GET",
                @"path":@"/api/audio/current-playback", @"body":body} owner:owner];
        };
        assert([page(@{@"offset":@1}, central.identifier)[@"status"] intValue] == 400);
        assert([page(@{@"offset":@-1}, central.identifier)[@"status"] intValue] == 400);
        assert([page(@{@"limit":@"bad"}, central.identifier)[@"status"] intValue] == 400);
        NSDictionary* first = page(@{}, central.identifier)[@"body"];
        NSString* transferId = first[@"transferId"];
        assert(transferId.length && [first[@"nextOffset"] intValue] == 4096);
        assert([page(@{@"transferId":transferId}, NSUUID.UUID)[@"status"] intValue] == 404);
        assert([page(@{@"transferId":transferId, @"offset":@40001}, central.identifier)[@"status"] intValue] == 416);
        assert(unlink(path) == 0);
        close(file);
        server->openPlayback = [](std::string&) { return -1; };
        NSMutableData* audio = [[NSMutableData alloc] initWithBase64EncodedString:first[@"dataBase64"] options:0];
        NSDictionary* next = first;
        while (![next[@"eof"] boolValue]) {
            next = page(@{@"transferId":transferId, @"offset":next[@"nextOffset"], @"limit":@999999}, central.identifier)[@"body"];
            assert(next && [next[@"transferId"] isEqual:transferId]);
            NSData* bytes = [[NSData alloc] initWithBase64EncodedString:next[@"dataBase64"] options:0];
            assert(bytes.length <= 16384);
            [audio appendData:bytes];
        }
        assert([audio isEqual:recording] && server.transfers.count == 0);
        assert([page(@{@"transferId":transferId}, central.identifier)[@"status"] intValue] == 404);
        assert([page(@{}, central.identifier)[@"status"] intValue] == 404);
        // Cancellation and idle expiry release descriptors, including unlinked files.
        CBMacTransfer* held = [CBMacTransfer new];
        held->fd = open("/dev/null", O_RDONLY);
        int heldFD = held->fd;
        held.owner = central.identifier;
        server.transfers[@"cancel"] = held;
        held = nil;
        assert([page(@{@"transferId":@"cancel", @"cancel":@YES}, central.identifier)[@"status"] intValue] == 200);
        assert(fcntl(heldFD, F_GETFD) == -1 && errno == EBADF);
        held = [CBMacTransfer new];
        held->fd = open("/dev/null", O_RDONLY);
        heldFD = held->fd;
        held->touched -= std::chrono::seconds(61);
        server.transfers[@"expired"] = held;
        held = nil;
        [server expireTransfers];
        assert(server.transfers.count == 0 && fcntl(heldFD, F_GETFD) == -1);
        for (int i = 0; i < 8; ++i) server.transfers[[@(i) stringValue]] = [CBMacTransfer new];
        assert([page(@{}, central.identifier)[@"status"] intValue] == 503);
        [server.transfers removeAllObjects];
        char largePath[] = "/tmp/channel-bank-ble-large-XXXXXX";
        int large = mkstemp(largePath);
        assert(large >= 0 && unlink(largePath) == 0);
        assert(ftruncate(large, 64 * 1024 * 1024 + 1) == 0);
        server->openPlayback = [large](std::string& name) { name = "large.wav"; return dup(large); };
        assert([page(@{}, central.identifier)[@"status"] intValue] == 413);
        assert(server.transfers.count == 0);
        close(large);
        testCompressedPlayback(server, central.identifier);
        testBinaryWindows(server, central, peripheral);
        std::cout << "Bluetooth framing, bounds, backpressure, routing, snapshot and audio lease tests passed\n";
    }
}
