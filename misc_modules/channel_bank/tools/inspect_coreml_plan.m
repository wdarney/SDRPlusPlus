// Build with clang -fobjc-arc -framework Foundation -framework CoreML.
// Usage: inspect_coreml_plan /path/to/encoder.mlmodelc
// This reports planned placement, not measured hardware execution.
#import <Foundation/Foundation.h>
#import <CoreML/CoreML.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s encoder.mlmodelc\n", argv[0]);
            return 2;
        }
        if (@available(macOS 14.4, *)) {
            MLModelConfiguration *config = [MLModelConfiguration new];
            config.computeUnits = MLComputeUnitsAll;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block int result = 1;
            [MLComputePlan loadContentsOfURL:[NSURL fileURLWithPath:@(argv[1])]
                configuration:config completionHandler:^(MLComputePlan *plan, NSError *error) {
                if (!plan || !plan.modelStructure.program) {
                    NSLog(@"Unable to inspect ML Program: %@", error);
                    dispatch_semaphore_signal(done);
                    return;
                }
                NSMutableDictionary *counts = [NSMutableDictionary new];
                NSInteger ane = 0, total = 0;
                for (MLModelStructureProgramFunction *function in plan.modelStructure.program.functions.allValues) {
                    // This converter emits a static graph without nested blocks.
                    for (MLModelStructureProgramOperation *op in function.block.operations) {
                        MLComputePlanDeviceUsage *usage = [plan computeDeviceUsageForMLProgramOperation:op];
                        if (!usage) continue;
                        NSString *key = NSStringFromClass([usage.preferredComputeDevice class]);
                        counts[key] = @([counts[key] integerValue] + 1);
                        ++total;
                        for (id device in usage.supportedComputeDevices) {
                            if ([device isKindOfClass:[MLNeuralEngineComputeDevice class]]) {
                                ++ane;
                                break;
                            }
                        }
                    }
                }
                NSLog(@"Planned preferred devices: %@; ANE supported operations: %ld/%ld",
                      counts, (long)ane, (long)total);
                result = 0;
                dispatch_semaphore_signal(done);
            }];
            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC))) {
                fprintf(stderr, "Timed out inspecting Core ML compute plan\n");
                return 1;
            }
            return result;
        }
        fprintf(stderr, "Compute-plan inspection requires macOS 14.4 or newer\n");
        return 2;
    }
}
