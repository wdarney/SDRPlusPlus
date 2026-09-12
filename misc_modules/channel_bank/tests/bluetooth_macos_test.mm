// Radio-free delegate tests: no CBPeripheralManager is created.
#include "../src/bluetooth_macos.mm"
#include <cassert>
#include <iostream>

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
@interface TestPeripheral : NSObject
@property CBATTError lastResult;
@property BOOL writable;
@property NSMutableArray<NSData*>* frames;
@end
@implementation TestPeripheral
- (void)respondToRequest:(CBATTRequest*)request withResult:(CBATTError)result { _lastResult = result; }
- (BOOL)updateValue:(NSData*)value forCharacteristic:(CBMutableCharacteristic*)characteristic onSubscribedCentrals:(NSArray*)centrals {
    if (!_writable) return NO;
    [_frames addObject:value];
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
        std::cout << "Bluetooth framing, bounds, backpressure, routing and snapshot tests passed\n";
    }
}
