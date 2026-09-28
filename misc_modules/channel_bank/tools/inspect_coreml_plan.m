// Build with clang -fobjc-arc -framework Foundation -framework CoreML.
// Usage: inspect_coreml_plan /path/to/encoder.mlmodelc [all|cpu-ane] [--require-ane]
// This reports planned placement, not measured hardware execution.
#import <Foundation/Foundation.h>
#import <CoreML/CoreML.h>
#include <string.h>

static NSArray<MLModelStructureProgram *> *programs(MLModelStructure *structure) API_AVAILABLE(macos(14.4));
static NSArray<MLModelStructureProgram *> *programs(MLModelStructure *structure) {
    NSMutableArray *result = [NSMutableArray new];
    if (structure.program) [result addObject:structure.program];
    for (MLModelStructure *child in structure.pipeline.subModels)
        [result addObjectsFromArray:programs(child)];
    return result;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 4 ||
            (argc >= 3 && strcmp(argv[2], "all") && strcmp(argv[2], "cpu-ane")) ||
            (argc == 4 && strcmp(argv[3], "--require-ane"))) {
            fprintf(stderr, "Usage: %s encoder.mlmodelc [all|cpu-ane] [--require-ane]\n", argv[0]);
            return 2;
        }
        if (@available(macOS 14.4, *)) {
            MLModelConfiguration *config = [MLModelConfiguration new];
            config.computeUnits = argc >= 3 && !strcmp(argv[2], "cpu-ane")
                ? MLComputeUnitsCPUAndNeuralEngine : MLComputeUnitsAll;
            const BOOL requireANE = argc == 4;
            NSLog(@"Available compute devices: %@; policy: %s", MLModel.availableComputeDevices,
                  argc >= 3 ? argv[2] : "all");
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block int result = 1;
            [MLComputePlan loadContentsOfURL:[NSURL fileURLWithPath:@(argv[1])]
                configuration:config completionHandler:^(MLComputePlan *plan, NSError *error) {
                if (!plan || programs(plan.modelStructure).count == 0) {
                    NSLog(@"Unable to inspect ML Program or pipeline: %@", error);
                    dispatch_semaphore_signal(done);
                    return;
                }
                NSMutableDictionary *counts = [NSMutableDictionary new];
                NSInteger ane = 0, preferredANE = 0, total = 0;
                for (MLModelStructureProgram *program in programs(plan.modelStructure))
                for (MLModelStructureProgramFunction *function in program.functions.allValues) {
                    // This converter emits a static graph without nested blocks.
                    for (MLModelStructureProgramOperation *op in function.block.operations) {
                        MLComputePlanDeviceUsage *usage = [plan computeDeviceUsageForMLProgramOperation:op];
                        if (!usage && [op.operatorName isEqualToString:@"const"]) continue;
                        NSString *key = usage.preferredComputeDevice
                            ? NSStringFromClass([usage.preferredComputeDevice class]) : @"Unreported";
                        counts[key] = @([counts[key] integerValue] + 1);
                        ++total;
                        if ([usage.preferredComputeDevice isKindOfClass:[MLNeuralEngineComputeDevice class]])
                            ++preferredANE;
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
                result = requireANE && (total == 0 || preferredANE != total || ane != total) ? 1 : 0;
                if (result) NSLog(@"Required full preferred ANE placement was not met");
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
