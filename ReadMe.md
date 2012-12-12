# FABatching

## Usage

### Header
    @interface MyClass
    ...
    @end

### Implementation
    #import "MyClass.h"
    #import "FABatching.h"
    
    @interface MyClass () {
        FA_BATCH_IVARS
    }
    @end
    @implementation
    FA_BATCH_IMPL(MyClass)
    - (void)dealloc
    {
        ..any code required to reset the instance's state..
        FA_BATCH_DEALLOC
    }
    @end

## Notes

FABatching is not compatible with ARC, you need to compile files using it, with `-fno-objc-arc`

When an instance is recycled, it's memory is not zeroed, so instance variables
from the recycled instance persist. You can take advantage of this fact like:

    - (id)init
    {
        if(!(self = [super init]))
            return nil;
        if(!_myIvar) // Only allocate a new dictionary if this is a fresh instance
            _myIvar = [NSMutableDictionary new];
        return self;
    }
    - (void)dealloc
    {
        [_myIvar removeAllObjects]; // Reset _myIvar
        FA_BATCH_DEALLOC
    }

Subclassing classes that use FABatching is tricky
and it is not possible for the subclass to add ivars(unless that class also
includes FA_BATCH_IMPL(MyClass), in which case you should be fine)
