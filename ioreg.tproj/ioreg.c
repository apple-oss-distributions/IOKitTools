/*
 * Copyright (c) 2000-2013 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CoreFoundation.h>            // (CFDictionary, ...)
#include <IOKit/IOCFSerialize.h>                      // (IOCFSerialize, ...)
#include <IOKit/IOKitLib.h>                           // (IOMasterPort, ...)
#include <IOKit/IOKitLibPrivate.h>                    // (IOServiceGetState, ...)
#include <IOKit/IOKitKeysPrivate.h>                   // (kIOClassNameOverrideNone, ...)
#include <sys/ioctl.h>                                // (TIOCGWINSZ, ...)
#include <term.h>                                     // (tputs, ...)
#include <unistd.h>                                   // (getopt, ...)

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define assertion_fatal(e, message) ((void) (__builtin_expect(!(e), 0) ? fprintf(stderr, "ioreg: error: %s.\n", message), exit(1) : 0))
#define assertion(e, message, fail) ((void) (__builtin_expect(!(e), 0) ? fprintf(stderr, "ioreg: error: %s.\n", message), (fail) : 0))

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct options
{
    UInt32 archive:1;                                 // (-a option)
    UInt32 bold:1;                                    // (-b option)
    UInt32 format:1;                                  // (-f option)
    UInt32 hex:1;                                     // (-x option)
    UInt32 inheritance:1;                             // (-i option)
    UInt32 list:1;                                    // (-l option)
    UInt32 root:1;                                    // (-r option)
    UInt32 tree:1;                                    // (-t option)

    char * class;                                     // (-c option)
    UInt32 depth;                                     // (-d option)
    char * key;                                       // (-k option)
    char * name;                                      // (-n option)
    char * plane;                                     // (-p option)
    UInt32 width;                                     // (-w option)
};

struct context
{
    io_registry_entry_t service;
    UInt32              serviceDepth;
    UInt64              stackOfBits;
    struct options      options;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void boldinit();
static void boldon();
static void boldoff();
static void printinit(int width);
static void print(const char * format, ...);
static void println(const char * format, ...);

static void cfshowinit(Boolean hex);
static void cfshow(CFTypeRef object);
static void cfarrayshow(CFArrayRef object);
static void cfbooleanshow(CFBooleanRef object);
static void cfdatashow(CFDataRef object);
static void cfdictionaryshow(CFDictionaryRef object);
static void cfnumbershow(CFNumberRef object);
static void cfsetshow(CFSetRef object);
static void cfstringshow(CFStringRef object);

static CFStringRef createInheritanceStringForIORegistryClassName(CFStringRef name);

static void printProp(CFStringRef key, CFTypeRef value, struct context * context);
static void printPhysAddr(CFTypeRef value, struct context * context);
static void printSlotNames(CFTypeRef value, struct context * context);
static void printPCIRanges(CFTypeRef value, struct context * context);
static void printInterruptMap(CFTypeRef value, struct context * context);
static void printInterrupts(CFTypeRef value, struct context * context);
static void printInterruptParent( CFTypeRef value, struct context * context );
static void printData(CFTypeRef value, struct context * context);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static CFMutableDictionaryRef archive( io_registry_entry_t service,
                                       struct options      options ) CF_RETURNS_RETAINED;

static CFMutableDictionaryRef archive_scan( io_registry_entry_t service,
                                            UInt32              serviceDepth,
                                            struct options      options ) CF_RETURNS_RETAINED;

static CFMutableArrayRef archive_search( io_registry_entry_t service,
                                         UInt32              serviceHasMatchedDepth,
                                         UInt32              serviceDepth,
                                         io_registry_entry_t stackOfObjects[],
                                         struct options      options ) CF_RETURNS_RETAINED;

static Boolean compare( io_registry_entry_t service,
                        struct options      options );

static void indent( Boolean isNode,
                    UInt32  serviceDepth,
                    UInt64  stackOfBits );

static void scan( io_registry_entry_t service,
                  Boolean             serviceHasMoreSiblings,
                  UInt32              serviceDepth,
                  UInt64              stackOfBits,
                  struct options      options );

static void search( io_registry_entry_t service,
                    UInt32              serviceHasMatchedDepth,
                    UInt32              serviceDepth,
                    io_registry_entry_t stackOfObjects[],
                    struct options      options );

static void show( io_registry_entry_t service,
                  UInt32              serviceDepth,
                  UInt64              stackOfBits,
                  struct options      options );

static void showitem( const void * key,
                      const void * value,
                      void *       parameter );

static void usage();

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int main(int argc, char ** argv)
{
    int                 argument = 0;
    CFWriteStreamRef    file     = 0; // (needs release)
    CFTypeRef           object   = 0; // (needs release)
    struct options      options;
    CFURLRef            path     = 0; // (needs release)
    io_registry_entry_t service  = 0; // (needs release)
    io_registry_entry_t stackOfObjects[64];
    Boolean             success  = FALSE;
    struct winsize      winsize;

    // Initialize our minimal state.

    options.archive     = FALSE;
    options.bold        = FALSE;
    options.format      = FALSE;
    options.hex         = FALSE;
    options.inheritance = FALSE;
    options.list        = FALSE;
    options.root        = FALSE;
    options.tree        = FALSE;

    options.class = 0;
    options.depth = 0;
    options.key   = 0;
    options.name  = 0;
    options.plane = kIOServicePlane;
    options.root  = 0;
    options.width = 0;

    // Obtain the screen width.

    if (isatty(fileno(stdout)))
    {
        if (ioctl(fileno(stdout), TIOCGWINSZ, &winsize) == 0)
        {
            options.width = winsize.ws_col;
        }
    }

    // Obtain the command-line arguments.

    while ( (argument = getopt(argc, argv, ":abc:d:fik:ln:p:rsStw:x")) != -1 )
    {
        switch (argument)
        {
            case 'a':
                options.archive = TRUE;
                break;
            case 'b':
                options.bold = TRUE;
                break;
            case 'c':
                options.class = optarg;
                break;
            case 'd':
                options.depth = atoi(optarg);
                break;
            case 'f':
                options.format = TRUE;
                break;
            case 'i':
                options.inheritance = TRUE;
                break;
            case 'k':
                options.key = optarg;
                break;
            case 'l':
                options.list = TRUE;
                break;
            case 'n':
                options.name = optarg;
                break;
            case 'p':
                options.plane = optarg;
                break;
            case 'r':
                options.root = TRUE;
                break;
            case 's':
                break;
            case 'S':
                break;
            case 't':
                options.tree = TRUE;
                break;
            case 'w':
                options.width = atoi(optarg);
                break;
            case 'x':
                options.hex = TRUE;
                break;
            default:
                usage();
                break;
        }
    }    

    // Initialize text output functions.

    cfshowinit(options.hex);

    printinit(options.width);

    if (options.bold)  boldinit();

    // Obtain the I/O Kit root service.

    service = IORegistryGetRootEntry(kIOMasterPortDefault);
    assertion_fatal(service, "can't obtain I/O Kit's root service");

    // Traverse over all the I/O Kit services.

    if (options.archive)
    {
        if (options.root)
        {
            object = archive_search( /* service                */ service,
                                     /* serviceHasMatchedDepth */ 0,
                                     /* serviceDepth           */ 0,
                                     /* stackOfObjects         */ stackOfObjects,
                                     /* options                */ options );
        }
        else
        {
            object = archive_scan( /* service      */ service,
                                   /* serviceDepth */ 0,
                                   /* options      */ options );
        }

        if (object)
        {
            path = CFURLCreateWithFileSystemPath( /* allocator   */ kCFAllocatorDefault,
                                                  /* filePath    */ CFSTR("/dev/stdout"),
                                                  /* pathStyle   */ kCFURLPOSIXPathStyle,
                                                  /* isDirectory */ FALSE );
            assertion_fatal(path != NULL, "can't create path");

            file = CFWriteStreamCreateWithFile(kCFAllocatorDefault, path);
            assertion_fatal(file != NULL, "can't create file");

            success = CFWriteStreamOpen(file);
            assertion_fatal(success, "can't open file");

            CFPropertyListWrite( /* propertyList */ object,
                                 /* stream       */ file,
                                 /* format       */ kCFPropertyListXMLFormat_v1_0,
                                 /* options      */ 0,
                                 /* error        */ NULL );

            CFWriteStreamClose(file);

            CFRelease(file);
            CFRelease(path);
            CFRelease(object);
        }
    }
    else
    {
        if (options.root)
        {
            search( /* service                */ service,
                    /* serviceHasMatchedDepth */ 0,
                    /* serviceDepth           */ 0,
                    /* stackOfObjects         */ stackOfObjects,
                    /* options                */ options );
        }
        else
        {
            scan( /* service                */ service,
                  /* serviceHasMoreSiblings */ FALSE,
                  /* serviceDepth           */ 0,
                  /* stackOfBits            */ 0,
                  /* options                */ options );
        }
    }

    // Release resources.

    IOObjectRelease(service);

    // Quit.

    exit(0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static CFMutableDictionaryRef archive( io_registry_entry_t service,
                                       struct options      options )
{
    io_name_t              class;          // (don't release)
    uint32_t               count      = 0;
    CFMutableDictionaryRef dictionary = 0; // (needs release)
    uint64_t               identifier = 0;
    io_name_t              location;       // (don't release)
    io_name_t              name;           // (don't release)
    CFTypeRef              object     = 0; // (needs release)
    uint64_t               state      = 0;
    kern_return_t          status     = KERN_SUCCESS;
    uint64_t               time       = 0;

    // Determine whether the service is a match.

    if (options.list || compare(service, options))
    {
        // Obtain the service's properties.

        status = IORegistryEntryCreateCFProperties( service,
                                                    &dictionary,
                                                    kCFAllocatorDefault,
                                                    kNilOptions );
        assertion(status == KERN_SUCCESS, "can't obtain properties", dictionary = NULL);
    }
    if (!dictionary)
    {
        dictionary = CFDictionaryCreateMutable( kCFAllocatorDefault,
                                                0,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks );
        assertion_fatal(dictionary != NULL, "can't create dictionary");
    }

    // Obtain the name of the service.

    status = IORegistryEntryGetNameInPlane(service, options.plane, name);
    assertion(status == KERN_SUCCESS, "can't obtain name", strcpy(name, "<name error>"));

    object = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    assertion_fatal(object != NULL, "can't create name");

    CFDictionarySetValue(dictionary, CFSTR("IORegistryEntryName"), object);
    CFRelease(object);

    // Obtain the location of the service.

    status = IORegistryEntryGetLocationInPlane(service, options.plane, location);
    if (status == KERN_SUCCESS)
    {
        object = CFStringCreateWithCString(kCFAllocatorDefault, location, kCFStringEncodingUTF8);
        assertion_fatal(object != NULL, "can't create location");

        CFDictionarySetValue(dictionary, CFSTR("IORegistryEntryLocation"), object);
        CFRelease(object);
    }

    // Obtain the ID of the service.

    status = IORegistryEntryGetRegistryEntryID(service, &identifier);
    assertion(status == KERN_SUCCESS, "can't obtain identifier", identifier = UINT64_MAX);

    object = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &identifier);
    assertion_fatal(object != NULL, "can't create identifier");

    CFDictionarySetValue(dictionary, CFSTR("IORegistryEntryID"), object);
    CFRelease(object);

    // Obtain the class of the service.

    status = _IOObjectGetClass(service, kIOClassNameOverrideNone, class);
    assertion(status == KERN_SUCCESS, "can't obtain class", strcpy(class, "<class error>"));

    object = CFStringCreateWithCString(kCFAllocatorDefault, class, kCFStringEncodingUTF8);
    assertion_fatal(object != NULL, "can't create class");

    CFDictionarySetValue(dictionary, CFSTR("IOObjectClass"), object);
    CFRelease(object);

    // Obtain the retain count of the service.

    count = IOObjectGetKernelRetainCount(service);

    object = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &count);
    assertion_fatal(object != NULL, "can't create retain count");

    CFDictionarySetValue(dictionary, CFSTR("IOObjectRetainCount"), object);
    CFRelease(object);

    // Obtain the busy state of the service (for IOService objects).

    if (_IOObjectConformsTo(service, "IOService", kIOClassNameOverrideNone))
    {
        status = IOServiceGetBusyStateAndTime(service, &state, &count, &time);
        assertion(status == KERN_SUCCESS, "can't obtain state", time = state = count = 0);

        object = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &state);
        assertion_fatal(object != NULL, "can't create state");

        CFDictionarySetValue(dictionary, CFSTR("IOServiceState"), object);
        CFRelease(object);

        object = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &count);
        assertion_fatal(object != NULL, "can't create busy state");

        CFDictionarySetValue(dictionary, CFSTR("IOServiceBusyState"), object);
        CFRelease(object);

        object = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &time);
        assertion_fatal(object != NULL, "can't create busy time");

        CFDictionarySetValue(dictionary, CFSTR("IOServiceBusyTime"), object);
        CFRelease(object);
    }

    return dictionary;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static CFMutableDictionaryRef archive_scan( io_registry_entry_t service,
                                            UInt32              serviceDepth,
                                            struct options      options )
{
    CFMutableArrayRef      array       = 0; // (needs release)
    io_registry_entry_t    child       = 0; // (needs release)
    io_registry_entry_t    childUpNext = 0; // (don't release)
    io_iterator_t          children    = 0; // (needs release)
    CFMutableDictionaryRef dictionary  = 0; // (needs release)
    CFTypeRef              object      = 0; // (needs release)
    kern_return_t          status      = KERN_SUCCESS;

    // Obtain the service's children.

    status = IORegistryEntryGetChildIterator(service, options.plane, &children);
    if (status == KERN_SUCCESS)
    {
        childUpNext = IOIteratorNext(children);

        // Obtain the relevant service information.

        dictionary = archive(service, options);

        // Traverse over the children of this service.

        if (options.depth == 0 || options.depth > serviceDepth + 1)
        {
            if (childUpNext)
            {
                array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
                assertion_fatal(array != NULL, "can't create array");

                while (childUpNext)
                {
                    child       = childUpNext;
                    childUpNext = IOIteratorNext(children);

                    object = archive_scan( /* service      */ child,
                                           /* serviceDepth */ serviceDepth + 1,
                                           /* options      */ options );
                    assertion_fatal(object != NULL, "can't obtain child");

                    CFArrayAppendValue(array, object);
                    CFRelease(object);

                    IOObjectRelease(child);
                }

                CFDictionarySetValue(dictionary, CFSTR("IORegistryEntryChildren"), array);
                CFRelease(array);
            }
        }

        IOObjectRelease(children);
    }

    return dictionary;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static CFMutableArrayRef archive_search( io_registry_entry_t service,
                                         UInt32              serviceHasMatchedDepth,
                                         UInt32              serviceDepth,
                                         io_registry_entry_t stackOfObjects[],
                                         struct options      options )
{
    CFMutableArrayRef      array       = 0; // (needs release)
    CFMutableArrayRef      array2      = 0; // (needs release)
    io_registry_entry_t    child       = 0; // (needs release)
    io_registry_entry_t    childUpNext = 0; // (don't release)
    io_iterator_t          children    = 0; // (needs release)
    CFMutableDictionaryRef dictionary  = 0; // (needs release)
    CFMutableDictionaryRef dictionary2 = 0; // (needs release)
    UInt32                 index       = 0;
    kern_return_t          status      = KERN_SUCCESS;

    // Determine whether the service is a match.

    if (serviceHasMatchedDepth < serviceDepth + 1 && compare(service, options))
    {
        if (options.depth)
        {
            serviceHasMatchedDepth = serviceDepth + options.depth;
        }
        else
        {
            serviceHasMatchedDepth = UINT32_MAX;
        }

        if (options.tree)
        {
            if (options.depth)  options.depth += serviceDepth;

            dictionary = archive_scan( /* service      */ service,
                                       /* serviceDepth */ serviceDepth,
                                       /* options      */ options );

            if (options.depth)  options.depth -= serviceDepth;

            for (index = serviceDepth; index > 0; index--)
            {
                dictionary2 = archive(stackOfObjects[index - 1], options);
                assertion_fatal(dictionary2 != NULL, "can't obtain parent");

                CFDictionarySetValue(dictionary2, CFSTR("IORegistryEntryChildren"), dictionary);
                CFRelease(dictionary);

                dictionary = dictionary2;
            }
        }
        else
        {
            dictionary = archive_scan( /* service      */ service,
                                       /* serviceDepth */ 0,
                                       /* options      */ options );
        }

        array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        assertion_fatal(array != NULL, "can't create array");

        CFArrayAppendValue(array, dictionary);
        CFRelease(dictionary);
    }

    // Save service into stackOfObjects for this depth.

    stackOfObjects[serviceDepth] = service;

    // Obtain the service's children.

    status = IORegistryEntryGetChildIterator(service, options.plane, &children);
    if (status == KERN_SUCCESS)
    {
        childUpNext = IOIteratorNext(children);

        // Traverse over the children of this service.

        while (childUpNext)
        {
            child       = childUpNext;
            childUpNext = IOIteratorNext(children);

            array2 = archive_search( /* service                */ child,
                                     /* serviceHasMatchedDepth */ serviceHasMatchedDepth,
                                     /* serviceDepth           */ serviceDepth + 1,
                                     /* stackOfObjects         */ stackOfObjects,
                                     /* options                */ options );
            if (array2)
            {
                if (array)
                {
                    CFArrayAppendArray(array, array2, CFRangeMake(0, CFArrayGetCount(array2)));
                    CFRelease(array2);
                }
                else
                {
                    array = array2;
                }
            }

            IOObjectRelease(child);
        }

        IOObjectRelease(children);
    }

    return array;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static Boolean compare( io_registry_entry_t service,
                        struct options      options )
{
    CFStringRef   key      = 0; // (needs release)
    io_name_t     location;     // (don't release)
    Boolean       match    = FALSE;
    io_name_t     name;         // (don't release)
    kern_return_t status   = KERN_SUCCESS;
    CFTypeRef     value    = 0; // (needs release)

    // Determine whether the class of the service is a match.

    if (options.class)
    {
        if (_IOObjectConformsTo(service, options.class, kIOClassNameOverrideNone) == FALSE)
        {
            return FALSE;
        }

        match = TRUE;
    }

    // Determine whether the key of the service is a match.

    if (options.key)
    {
        key = CFStringCreateWithCString( kCFAllocatorDefault,
                                         options.key,
                                         kCFStringEncodingUTF8 );
        assertion_fatal(key != NULL, "can't create key");

        value = IORegistryEntryCreateCFProperty( service,
                                                 key,
                                                 kCFAllocatorDefault,
                                                 kNilOptions );

        CFRelease(key);

        if (value == NULL)
        {
            return FALSE;
        }

        CFRelease(value);

        match = TRUE;
    }

    // Determine whether the name of the service is a match.

    if (options.name)
    {
        // Obtain the name of the service.

        status = IORegistryEntryGetNameInPlane(service, options.plane, name);
        assertion(status == KERN_SUCCESS, "can't obtain name", strcpy(name, "<name error>"));

        if (strchr(options.name, '@'))
        {
            strlcat(name, "@", sizeof(name));

            // Obtain the location of the service.

            status = IORegistryEntryGetLocationInPlane(service, options.plane, location);
            if (status == KERN_SUCCESS)  strlcat(name, location, sizeof(name));
        }

        if (strcmp(options.name, name))
        {
            return FALSE;
        }

        match = TRUE;
    }

    return match;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void scan( io_registry_entry_t service,
                  Boolean             serviceHasMoreSiblings,
                  UInt32              serviceDepth,
                  UInt64              stackOfBits,
                  struct options      options )
{
    io_registry_entry_t child       = 0; // (needs release)
    io_registry_entry_t childUpNext = 0; // (don't release)
    io_iterator_t       children    = 0; // (needs release)
    kern_return_t       status      = KERN_SUCCESS;

    // Obtain the service's children.

    status = IORegistryEntryGetChildIterator(service, options.plane, &children);
    if (status == KERN_SUCCESS)
    {
        childUpNext = IOIteratorNext(children);

        // Save has-more-siblings state into stackOfBits for this depth.

        if (serviceHasMoreSiblings)
            stackOfBits |=  (1 << serviceDepth);
        else
            stackOfBits &= ~(1 << serviceDepth);

        // Save has-children state into stackOfBits for this depth.

        if (options.depth == 0 || options.depth > serviceDepth + 1)
        {
            if (childUpNext)
                stackOfBits |=  (2 << serviceDepth);
            else
                stackOfBits &= ~(2 << serviceDepth);
        }

        // Print out the relevant service information.

        show(service, serviceDepth, stackOfBits, options);

        // Traverse over the children of this service.

        if (options.depth == 0 || options.depth > serviceDepth + 1)
        {
            while (childUpNext)
            {
                child       = childUpNext;
                childUpNext = IOIteratorNext(children);

                scan( /* service                */ child,
                      /* serviceHasMoreSiblings */ (childUpNext) ? TRUE : FALSE,
                      /* serviceDepth           */ serviceDepth + 1,
                      /* stackOfBits            */ stackOfBits,
                      /* options                */ options );

                IOObjectRelease(child);
            }
        }

        IOObjectRelease(children);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void search( io_registry_entry_t service,
                    UInt32              serviceHasMatchedDepth,
                    UInt32              serviceDepth,
                    io_registry_entry_t stackOfObjects[],
                    struct options      options )
{
    io_registry_entry_t child       = 0; // (needs release)
    io_registry_entry_t childUpNext = 0; // (don't release)
    io_iterator_t       children    = 0; // (needs release)
    UInt32              index       = 0;
    kern_return_t       status      = KERN_SUCCESS;

    // Determine whether the service is a match.

    if (serviceHasMatchedDepth < serviceDepth + 1 && compare(service, options))
    {
        if (options.depth)
        {
            serviceHasMatchedDepth = serviceDepth + options.depth;
        }
        else
        {
            serviceHasMatchedDepth = UINT32_MAX;
        }

        if (options.tree)
        {
            for (index = 0; index < serviceDepth; index++)
            {
                show(stackOfObjects[index], index, (2 << index), options);
            }

            if (options.depth)  options.depth += serviceDepth;

            scan( /* service                */ service,
                  /* serviceHasMoreSiblings */ FALSE,
                  /* serviceDepth           */ serviceDepth,
                  /* stackOfBits            */ 0,
                  /* options                */ options );

            if (options.depth)  options.depth -= serviceDepth;
        }
        else
        {
            scan( /* service                */ service,
                  /* serviceHasMoreSiblings */ FALSE,
                  /* serviceDepth           */ 0,
                  /* stackOfBits            */ 0,
                  /* options                */ options );
        }

        println("");
    }

    // Save service into stackOfObjects for this depth.

    stackOfObjects[serviceDepth] = service;

    // Obtain the service's children.

    status = IORegistryEntryGetChildIterator(service, options.plane, &children);
    if (status == KERN_SUCCESS)
    {
        childUpNext = IOIteratorNext(children);

        // Traverse over the children of this service.

        while (childUpNext)
        {
            child       = childUpNext;
            childUpNext = IOIteratorNext(children);

            search( /* service                */ child,
                    /* serviceHasMatchedDepth */ serviceHasMatchedDepth,
                    /* serviceDepth           */ serviceDepth + 1,
                    /* stackOfObjects         */ stackOfObjects,
                    /* options                */ options );

            IOObjectRelease(child);
        }

        IOObjectRelease(children);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void show( io_registry_entry_t service,
                  UInt32              serviceDepth,
                  UInt64              stackOfBits,
                  struct options      options )
{
    io_name_t              class;          // (don't release)
    struct context         context    = { service, serviceDepth, stackOfBits, options };
    uint32_t               integer    = 0;
    uint64_t               state      = 0;
    uint64_t               accumulated_busy_time;
    io_name_t              location;       // (don't release)
    io_name_t              name;           // (don't release)
    CFMutableDictionaryRef properties = 0; // (needs release)
    kern_return_t          status     = KERN_SUCCESS;

    // Print out the name of the service.

    status = IORegistryEntryGetNameInPlane(service, options.plane, name);
    assertion(status == KERN_SUCCESS, "can't obtain name", strcpy(name, "<name error>"));

    indent(TRUE, serviceDepth, stackOfBits);

    if (options.bold)  boldon();

    print("%s", name);

    if (options.bold)  boldoff();

    // Print out the location of the service.

    status = IORegistryEntryGetLocationInPlane(service, options.plane, location);
    if (status == KERN_SUCCESS)  print("@%s", location);

    // Print out the class of the service.

    print("  <class ");

    if (options.inheritance)
    {
        CFStringRef classCFStr;
        CFStringRef ancestryCFStr;
        char *      aCStr;

        classCFStr = IOObjectCopyClass (service);
        if (classCFStr) {
            ancestryCFStr = createInheritanceStringForIORegistryClassName (classCFStr);
            if (ancestryCFStr) {
                aCStr = (char *) CFStringGetCStringPtr (ancestryCFStr, kCFStringEncodingMacRoman);
                if (NULL != aCStr)
                {
                    print(aCStr);
                }
                CFRelease (ancestryCFStr);
            }
            CFRelease (classCFStr);
        }
    }
    else
    {
        status = _IOObjectGetClass(service, kIOClassNameOverrideNone, class);
        assertion(status == KERN_SUCCESS, "can't obtain class", strcpy(class, "<class error>"));

        print("%s", class);
    }

    // Prepare to print out the service's useful debug information.

    uint64_t entryID;

    status = IORegistryEntryGetRegistryEntryID(service, &entryID);
    if (status == KERN_SUCCESS)
    {
        print(", id 0x%llx", entryID);
    }

    // Print out the busy state of the service (for IOService objects).

    if (_IOObjectConformsTo(service, "IOService", kIOClassNameOverrideNone))
    {
        status = IOServiceGetBusyStateAndTime(service, &state, &integer, &accumulated_busy_time);
        assertion(status == KERN_SUCCESS, "can't obtain state", accumulated_busy_time = integer = state = 0);

        print( ", %sregistered, %smatched, %sactive",
               state & kIOServiceRegisteredState ? "" : "!",
               state & kIOServiceMatchedState    ? "" : "!",
               state & kIOServiceInactiveState   ? "in" : "" );

        print(", busy %ld", 
        (unsigned long)integer);

        if (accumulated_busy_time)
        {
            print(" (%lld ms)", 
            accumulated_busy_time / kMillisecondScale);
        }
    }

    // Print out the retain count of the service.

    integer = IOObjectGetKernelRetainCount(service);

    print(", retain %ld", (unsigned long)integer);

    println(">");

    // Determine whether the service is a match.

    if (options.list || compare(service, options))
    {
        indent(FALSE, serviceDepth, stackOfBits);
        println("{");

        // Obtain the service's properties.

        status = IORegistryEntryCreateCFProperties( service,
                                                    &properties,
                                                    kCFAllocatorDefault,
                                                    kNilOptions );
        assertion(status == KERN_SUCCESS, "can't obtain properties", properties = NULL);

        if (!properties)
        {
            properties = CFDictionaryCreateMutable( kCFAllocatorDefault,
                                                    0,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks );
            assertion_fatal(properties != NULL, "can't create dictionary");
        }

        // Print out the service's properties.

        CFDictionaryApplyFunction(properties, showitem, &context);

        indent(FALSE, serviceDepth, stackOfBits);
        println("}");
        indent(FALSE, serviceDepth, stackOfBits);
        println("");

        // Release resources.

        CFRelease(properties);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void showitem(const void * key, const void * value, void * parameter)
{
    struct context * context = parameter; // (don't release)

    // Print out one of the service's properties.

    indent(FALSE, context->serviceDepth, context->stackOfBits);
    print("  ");
    cfshow(key);
    print(" = ");

    if (context->options.format)
    {
        printProp(key, value, context);
    }
    else
    {
        cfshow(value);
        println("");
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void indent(Boolean isNode, UInt32 serviceDepth, UInt64 stackOfBits)
{
    // stackOfBits representation, given current zero-based depth is n:
    //   bit n+1             = does depth n have children?       1=yes, 0=no
    //   bit [n, .. i .., 0] = does depth i have more siblings?  1=yes, 0=no

    UInt32 index;

    if (isNode)
    {
        for (index = 0; index < serviceDepth; index++)
            print( (stackOfBits & (1 << index)) ? "| " : "  " );

        print("+-o ");
    }
    else // if (!isNode)
    {
        for (index = 0; index <= serviceDepth + 1; index++)
            print( (stackOfBits & (1 << index)) ? "| " : "  " );
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void usage()
{
    fprintf( stderr,
     "usage: ioreg [-abfilrtx] [-c class] [-d depth] [-k key] [-n name] [-p plane] [-w width]\n"
     "where options are:\n"
     "\t-a archive output\n"
     "\t-b show object name in bold\n"
     "\t-c list properties of objects with the given class\n"
     "\t-d limit tree to the given depth\n"
     "\t-f enable smart formatting\n"
     "\t-i show object inheritance\n"
     "\t-k list properties of objects with the given key\n"
     "\t-l list properties of all objects\n"
     "\t-n list properties of objects with the given name\n"
     "\t-p traverse registry over the given plane (IOService is default)\n"
     "\t-r show subtrees rooted by the given criteria\n"
     "\t-t show location of each subtree\n"
     "\t-w clip output to the given line width (0 is unlimited)\n"
     "\t-x show data and numbers as hexadecimal\n"
     );
    exit(1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char * termcapstr_boldon  = 0;
static char * termcapstr_boldoff = 0;

static int termcapstr_outc(int c)
{
    return putchar(c);
}

static void boldinit()
{
    char *      term;
    static char termcapbuf[64];
    char *      termcapbufptr = termcapbuf;

    term = getenv("TERM");

    if (term)
    {
        if (tgetent(NULL, term) > 0)
        {
            termcapstr_boldon  = tgetstr("md", &termcapbufptr);
            termcapstr_boldoff = tgetstr("me", &termcapbufptr);

            assertion_fatal(termcapbufptr - termcapbuf <= sizeof(termcapbuf), "can't obtain terminfo");
        }
    }

    if (termcapstr_boldon  == 0)  termcapstr_boldon  = "";
    if (termcapstr_boldoff == 0)  termcapstr_boldoff = "";
}

static void boldon()
{
    tputs(termcapstr_boldon, 1, termcapstr_outc);
}

static void boldoff()
{
    tputs(termcapstr_boldoff, 1, termcapstr_outc);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char * printbuf     = 0;
static int    printbufclip = FALSE;
static int    printbufleft = 0;
static int    printbufsize = 0;

static void printinit(int width)
{
    if (width)
    {
        printbuf     = malloc(width);
        printbufleft = width;
        printbufsize = width;

        assertion_fatal(printbuf != NULL, "can't allocate buffer");
    }
}

static void printva(const char * format, va_list arguments)
{
    if (printbufsize)
    {
        char * c;
        int    count = vsnprintf(printbuf, printbufleft, format, arguments);

        while ( (c = strchr(printbuf, '\n')) )  *c = ' ';    // (strip newlines)

        printf("%s", printbuf);

        if (count >= printbufleft)
        {
            count = printbufleft - 1;
            printbufclip = TRUE;
        }

        printbufleft -= count;   // (printbufleft never hits zero, stops at one)
    }
    else
    {
        vprintf(format, arguments);
    }
}

static void print(const char * format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printva(format, arguments);
    va_end(arguments);
}

static void println(const char * format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printva(format, arguments);
    va_end(arguments);

    if (printbufclip)  printf("$");

    printf("\n");

    printbufclip = FALSE;
    printbufleft = printbufsize;
}    

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static Boolean cfshowhex;

static void cfshowinit(Boolean hex)
{
    cfshowhex = hex;
}

static void cfshow(CFTypeRef object)
{
    CFTypeID type = CFGetTypeID(object);

    if      ( type == CFArrayGetTypeID()      )  cfarrayshow(object);
    else if ( type == CFBooleanGetTypeID()    )  cfbooleanshow(object);
    else if ( type == CFDataGetTypeID()       )  cfdatashow(object);
    else if ( type == CFDictionaryGetTypeID() )  cfdictionaryshow(object);
    else if ( type == CFNumberGetTypeID()     )  cfnumbershow(object);
    else if ( type == CFSetGetTypeID()        )  cfsetshow(object);
    else if ( type == CFStringGetTypeID()     )  cfstringshow(object);
    else print("<unknown object>");
}

static void cfarrayshowapplier(const void * value, void * parameter)
{
    Boolean * first = (Boolean *) parameter;

    if (*first)
        *first = FALSE;
    else
        print(",");

    cfshow(value);
}

static void cfarrayshow(CFArrayRef object)
{
    Boolean first = TRUE;
    CFRange range = { 0, CFArrayGetCount(object) };

    print("(");
    CFArrayApplyFunction(object, range, cfarrayshowapplier, &first);
    print(")");
}

static void cfbooleanshow(CFBooleanRef object)
{
    print(CFBooleanGetValue(object) ? "Yes" : "No");
}

static void cfdatashow(CFDataRef object)
{
    UInt32        asciiNormalCount = 0;
    UInt32        asciiSymbolCount = 0;
    const UInt8 * bytes;
    CFIndex       index;
    CFIndex       length;

    print("<");
    length = CFDataGetLength(object);
    bytes  = CFDataGetBytePtr(object);

    //
    // This algorithm detects ascii strings, or a set of ascii strings, inside a
    // stream of bytes.  The string, or last string if in a set, needn't be null
    // terminated.  High-order symbol characters are accepted, unless they occur
    // too often (80% of characters must be normal).  Zero padding at the end of
    // the string(s) is valid.  If the data stream is only one byte, it is never
    // considered to be a string.
    //

    for (index = 0; index < length; index++)  // (scan for ascii string/strings)
    {
        if (bytes[index] == 0)       // (detected null in place of a new string,
        {                            //  ensure remainder of the string is null)
            break;          // (either end of data or a non-null byte in stream)
        }
        else                         // (scan along this potential ascii string)
        {
            for (; index < length; index++)
            {
                if (isprint(bytes[index]))
                    asciiNormalCount++;
                else if (bytes[index] >= 128 && bytes[index] <= 254)
                    asciiSymbolCount++;
                else
                    break;
            }

            if (index < length && bytes[index] == 0)          // (end of string)
                continue;
            else             // (either end of data or an unprintable character)
                break;
        }
    }

    if ((asciiNormalCount >> 2) < asciiSymbolCount)    // (is 80% normal ascii?)
        index = 0;
    else if (length == 1)                                 // (is just one byte?)
        index = 0;
    else if (cfshowhex)
        index = 0;

    if (index >= length && asciiNormalCount) // (is a string or set of strings?)
    {
        Boolean quoted = FALSE;

        for (index = 0; index < length; index++)
        {
            if (bytes[index])
            {
                if (quoted == FALSE)
                {
                    quoted = TRUE;
                    if (index)
                        print(",\"");
                    else
                        print("\"");
                }
                print("%c", bytes[index]);
            }
            else
            {
                if (quoted == TRUE)
                {
                    quoted = FALSE;
                    print("\"");
                }
                else
                    break;
            }
        }
        if (quoted == TRUE)
            print("\"");
    }
    else                                  // (is not a string or set of strings)
    {
        for (index = 0; index < length; index++)  print("%02x", bytes[index]);
    }

    print(">");
}

static void cfdictionaryshowapplier( const void * key,
                                     const void * value,
                                     void *       parameter )
{
    Boolean * first = (Boolean *) parameter;

    if (*first)
        *first = FALSE;
    else
        print(",");

    cfshow(key);
    print("=");
    cfshow(value);
}

static void cfdictionaryshow(CFDictionaryRef object)
{
    Boolean first = TRUE;

    print("{");
    CFDictionaryApplyFunction(object, cfdictionaryshowapplier, &first);
    print("}");
}

static void cfnumbershow(CFNumberRef object)
{
    long long number;

    if (CFNumberGetValue(object, kCFNumberLongLongType, &number))
    {
        if (cfshowhex)
            print("0x%qx", (unsigned long long)number); 
        else
            print("%qu", (unsigned long long)number); 
    }
}

static void cfsetshowapplier(const void * value, void * parameter)
{
    Boolean * first = (Boolean *) parameter;

    if (*first)
        *first = FALSE;
    else
        print(",");

    cfshow(value);
}

static void cfsetshow(CFSetRef object)
{
    Boolean first = TRUE;
    print("[");
    CFSetApplyFunction(object, cfsetshowapplier, &first);
    print("]");
}

static void cfstringshow(CFStringRef object)
{
    const char * c = CFStringGetCStringPtr(object, kCFStringEncodingMacRoman);

    if (c)
        print("\"%s\"", c);
    else
    {
        CFIndex bufferSize = CFStringGetLength(object) + 1;
        char *  buffer     = malloc(bufferSize);

        if (buffer)
        {
            if ( CFStringGetCString(
                    /* string     */ object,
                    /* buffer     */ buffer,
                    /* bufferSize */ bufferSize,
                    /* encoding   */ kCFStringEncodingMacRoman ) )
                print("\"%s\"", buffer);

            free(buffer);
        }
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static CFStringRef createInheritanceStringForIORegistryClassName(CFStringRef name)
{
	CFStringRef				curClassCFStr = NULL;
	CFStringRef				oldClassCFStr = NULL;
	CFMutableStringRef		outCFStr = NULL;
	
	outCFStr = CFStringCreateMutable (NULL, 512);
    if (outCFStr == NULL) return(NULL);
    
    CFStringInsert (outCFStr, 0, name);
    
    curClassCFStr = CFStringCreateCopy (NULL, name);
    if (curClassCFStr == NULL) return((CFStringRef) outCFStr);

    for (;;)
    {
        oldClassCFStr = curClassCFStr;
        curClassCFStr = IOObjectCopySuperclassForClass (curClassCFStr);
        CFRelease (oldClassCFStr);
        if (curClassCFStr == NULL)  break;
        
        if (FALSE == CFEqual (curClassCFStr, CFSTR ("OSObject")))
        {
            CFStringInsert (outCFStr, 0, CFSTR (":"));
            CFStringInsert (outCFStr, 0, curClassCFStr);
        }
        else
        {
            break;
        }
    } // for...
    
    if (curClassCFStr) CFRelease(curClassCFStr);
	
	// Return the CFMutableStringRef as a CFStringRef because it is derived and compatible:
	return (CFStringRef) outCFStr;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void printProp(CFStringRef key, CFTypeRef value, struct context * context)
{
    kern_return_t       status     = KERN_SUCCESS;
    Boolean             valueShown = FALSE;  // Flag is set when property is printed
    io_registry_entry_t thisObj;
    
    thisObj = context->service;
    
	// Match "reg" property for PCI devices. 
	if (CFStringCompare(key, CFSTR("reg"), 0 ) == 0)
	{
		io_registry_entry_t parentObj;  // (needs release)
		io_name_t parentName;
		
		// If the parent entry in the IODeviceTree plane is "pci",
		// then we've found what we're looking for.
		
		status = IORegistryEntryGetParentEntry( thisObj,
												kIODeviceTreePlane,
												&parentObj );
		if (status == KERN_SUCCESS)
		{
            status = IORegistryEntryGetNameInPlane( parentObj,
                                                    kIODeviceTreePlane,
                                                    parentName );
            assertion(status == KERN_SUCCESS, "could not get name of parent", strcpy(parentName, "<name error>"));
            
            IOObjectRelease(parentObj);
            
            if (strncmp(parentName, "pci", 3) == 0)
            {
                printPhysAddr(value, context);
                valueShown = TRUE;
            }
		}
	}
	
	// Match "assigned-addresses" property.
	else if (CFStringCompare(key, CFSTR("assigned-addresses"), 0) == 0)
	{
		printPhysAddr(value, context);
		valueShown = TRUE;
	}
	
	// Match "slot-names" property.
	else if (CFStringCompare(key, CFSTR("slot-names"), 0) == 0)
	{
		printSlotNames(value, context);
		valueShown = TRUE;
	}

	// Match "ranges" property.
	else if (CFStringCompare(key, CFSTR("ranges"), 0) == 0)
	{            
		printPCIRanges(value, context);
		valueShown = TRUE;
	}
	
	// Match "interrupt-map" property.
	else if (CFStringCompare(key, CFSTR("interrupt-map"), 0) == 0)
	{
		printInterruptMap(value, context);
		valueShown = TRUE;
	}

	// Match "interrupts" property.
	else if ( CFStringCompare( key, CFSTR("interrupts"), 0) == 0 )
	{
		printInterrupts( value, context );
		valueShown = TRUE;
	}

	// Match "interrupt-parent" property.
	else if ( CFStringCompare( key, CFSTR("interrupt-parent"), 0) == 0 )
	{
		printInterruptParent( value, context );
		valueShown = TRUE;
	}

    // Print the value if it doesn't have a formatter.
    if (valueShown == FALSE)
    {
        if (CFGetTypeID(value) == CFDataGetTypeID())
        {
            printData(value, context);
        }
        else
        {
            cfshow(value);
            println("");
        }
    }
}

/* The following data structures, masks and shift values are used to decode
 * physical address properties as defined by IEEE 1275-1994.  The format is
 * used in 'reg' and 'assigned-address' properties.
 *
 * The format of the physHi word is as follows:
 *
 * npt000ss bbbbbbbb dddddfff rrrrrrrr
 *
 * n         1 = Relocatable, 0 = Absolute               (1 bit)
 * p         1 = Prefetchable                            (1 bit)
 * t         1 = Alias                                   (1 bit)
 * ss        Space code (Config, I/O, Mem, 64-bit Mem)   (2 bits)
 * bbbbbbbb  Bus number                                  (8 bits)
 * ddddd     Device number                               (5 bits)
 * fff       Function number                             (3 bits)
 * rrrrrrrr  Register number                             (8 bits)
 */
 
struct physAddrProperty {
    UInt32  physHi;
    UInt32  physMid;
    UInt32  physLo;
    UInt32  sizeHi;
    UInt32  sizeLo;
};

#define kPhysAbsoluteMask   0x80000000
#define kPhysPrefetchMask   0x40000000
#define kPhysAliasMask      0x20000000
#define kPhysSpaceMask      0x03000000
#define kPhysSpaceShift     24
#define kPhysBusMask        0x00FF0000
#define kPhysBusShift       16
#define kPhysDeviceMask     0x0000F800
#define kPhysDeviceShift    11
#define kPhysFunctionMask   0x00000700
#define kPhysFunctionShift  8
#define kPhysRegisterMask   0x000000FF
#define kPhysRegisterShift  0

static SInt32
getRecursivePropValue( io_registry_entry_t thisRegEntry, CFStringRef propertyNameToLookFor )
{
	SInt32		returnValue;
	CFTypeRef	ptr;

    ptr = IORegistryEntrySearchCFProperty(thisRegEntry,
                                          kIODeviceTreePlane,
                                          propertyNameToLookFor,
                                          kCFAllocatorDefault,
                                          kIORegistryIterateParents | kIORegistryIterateRecursively);
    assertion( ptr != NULL, "unable to get properties", returnValue = 0 );
    if (ptr)
    {
        returnValue = *(SInt32 *)CFDataGetBytePtr( (CFDataRef) ptr );
        CFRelease( ptr );
    }
	return( returnValue );
}

static void printPhysAddr(CFTypeRef value, struct context * context)
{
    CFIndex length;                      // stores total byte count in this prop.
    struct physAddrProperty *physAddr;   // points to current physAddr property
    UInt64 numPhysAddr,                  // how many physAddr's to decode?
           count;                        // loop counter variable
    UInt32 tmpCell;                      // temp storage for a single word
           
    UInt32 busNumber,                    // temp storage for decoded values
           deviceNumber,
           functionNumber,
           registerNumber;
    const char *addressType,
               *isPrefetch,
               *isAlias,
               *isAbsolute;

    // Ensure that the object passed in is in fact a CFData object.

    assertion_fatal(CFGetTypeID(value) == CFDataGetTypeID(), "invalid phys addr");

    // Make sure there is actually data in the object.
    length = CFDataGetLength((CFDataRef)value);
    
    if (length == 0)
    {
        println("<>");
        return;
    }

    numPhysAddr = length / sizeof(struct physAddrProperty);
    physAddr = (struct physAddrProperty *)CFDataGetBytePtr((CFDataRef)value);

    println("");

    for (count = 0; count < numPhysAddr; count++)
    {
        tmpCell = physAddr[count].physHi;  // copy physHi word to a temp var

        // Decode the fields in the physHi word.

        busNumber      = (tmpCell & kPhysBusMask) >> kPhysBusShift;
        deviceNumber   = (tmpCell & kPhysDeviceMask) >> kPhysDeviceShift; 
        functionNumber = (tmpCell & kPhysFunctionMask) >> kPhysFunctionShift;
        registerNumber = (tmpCell & kPhysRegisterMask) >> kPhysRegisterShift;
        isAbsolute     = ((tmpCell & kPhysAbsoluteMask) != 0) ? "abs" : "rel";
        isPrefetch     = ((tmpCell & kPhysPrefetchMask) != 0) ? ", prefetch" : "";
        isAlias        = ((tmpCell & kPhysAliasMask) != 0) ? ", alias" : "";
        switch ((tmpCell & kPhysSpaceMask) >> kPhysSpaceShift)
        {
            case 0:  addressType = "Config"; break;
            case 1:  addressType = "I/O";    break;
            case 2:  addressType = "Mem";    break;
            case 3:  addressType = "64-bit"; break;
            default: addressType = "?";      break;
        }
        
        // Format and print the information for this entry.
        
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        println("    %02lu: phys.hi: %08lx phys.mid: %08lx phys.lo: %08lx",
                (unsigned long)count,
                (unsigned long)physAddr[count].physHi,
                (unsigned long)physAddr[count].physMid,
                (unsigned long)physAddr[count].physLo );

        indent(FALSE, context->serviceDepth, context->stackOfBits);
        println("        size.hi: %08lx size.lo: %08lx",
                (unsigned long)physAddr[count].sizeHi,
                (unsigned long)physAddr[count].sizeLo );

        indent(FALSE, context->serviceDepth, context->stackOfBits);
        println("        bus: %lu dev: %lu func: %lu reg: %lu",
                (unsigned long)busNumber,
                (unsigned long)deviceNumber,
                (unsigned long)functionNumber,
                (unsigned long)registerNumber );

        indent(FALSE, context->serviceDepth, context->stackOfBits);
        println("        type: %s flags: %s%s%s",
                addressType,
                isAbsolute,
                isPrefetch,
                isAlias );
    }
}

static void printSlotNames(CFTypeRef value, struct context * context)
{
    CFIndex length;
    char * bytePtr;
    UInt32 count;
    UInt32 * avail_slots;
    
    // Ensure that the object passed in is in fact a CFData object.

    assertion_fatal(CFGetTypeID(value) == CFDataGetTypeID(), "invalid phys addr");

    // Make sure there is actually data in the object.
    
    length = CFDataGetLength((CFDataRef)value);
    
    if (length == 0)
    {
        println("<>");
        return;
    }

    avail_slots = (UInt32 *)CFDataGetBytePtr((CFDataRef)value);
    bytePtr = (char *)avail_slots + sizeof(UInt32);

    // Ignore entries that have no named slots.
    
    if (*avail_slots == 0)
    {
        println("<>");
        return;
    }

    println("");

    // Cycle through all 32 bit positions and print slot names.

    for (count = 0; count < 32; count++)
    {
        if ((*avail_slots & (1 << count)) != 0)
        {
            indent(FALSE, context->serviceDepth, context->stackOfBits);
            println("    %02lu: %s", (unsigned long)count, bytePtr);
            bytePtr += strlen(bytePtr) + 1;  // advance to next string
        }
    }
}

static void printPCIRanges(CFTypeRef value, struct context * context)
{
    kern_return_t		   status = KERN_SUCCESS;
    CFIndex 			   length;
    UInt32                 *quadletPtr;
    SInt32				   parentACells, childACells, childSCells, elemSize;
    io_registry_entry_t    parentObj;   // must be released
    UInt64                 i,j,nRanges;
    SInt32				   counts[3];
    const char			   *titles[] = {"-child--", "-parent-", "-size---"};
    
    // Ensure that the object passed in is in fact a CFData object.
    assertion_fatal(CFGetTypeID(value) == CFDataGetTypeID(), "invalid ranges");

    // Make sure there is actually data in the object.
    length = CFDataGetLength((CFDataRef)value);
    
    if (length == 0)
    {
        println("<>");
        return;
    }

    quadletPtr = (UInt32 *)CFDataGetBytePtr((CFDataRef)value);

    // Get #address-cells of device-tree parent
    status = IORegistryEntryGetParentEntry( context->service, kIODeviceTreePlane, &parentObj );
    assertion(status == KERN_SUCCESS, "unable to get device tree parent", parentObj = MACH_PORT_NULL);

	parentACells = getRecursivePropValue( parentObj, CFSTR( "#address-cells" ) );

    IOObjectRelease( parentObj );

    // Get #address-cells and #size-cells for owner
	childACells = getRecursivePropValue( context->service, CFSTR( "#address-cells" ) );
	childSCells = getRecursivePropValue( context->service, CFSTR( "#size-cells"    ) );
    
    // ranges property is a list of [[child addr][parent addr][size]]
    elemSize = childACells + parentACells + childSCells;

    // print a title line
    println("");
    indent(FALSE, context->serviceDepth, context->stackOfBits);
    print("    ");

    // set up array of cell counts (only used to print title)
    counts[0] = childACells;
    counts[1] = parentACells;
    counts[2] = childSCells;
    
    for (j = 0; j < 3; j++)
    {
        print("%s", titles[j]);  // titles is init'ed at start of func.
        if (counts[j] > 1)
        {
            print("-");
            for( i = 2; i <= counts[j]; i++)
            {
                if(i == counts[j])
                    print("-------- ");
                else
                    print("---------");
            }
        }
        else
            print(" ");
    }
    println("");

    nRanges = length/(elemSize * sizeof(UInt32));

    for(j = 0; j < nRanges; j++)
    {
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        print("    ");
        for(i = 0; i < elemSize; i++) print("%08lx ", (unsigned long)*quadletPtr++);
        println("");
    }
}

// constructs a path string for a node in the device tree
static void makepath(io_registry_entry_t target, io_string_t path)
{
    kern_return_t status = KERN_SUCCESS;
    
    status = IORegistryEntryGetPath(target, kIODeviceTreePlane, path);
    assertion(status == KERN_SUCCESS, "unable to get path", strcpy(path, "<path error"));

    memmove(path, strchr(path, ':') + 1, strlen(strchr(path, ':') + 1) + 1);
}

static Boolean lookupPHandle(UInt32 phandle, io_registry_entry_t * device)
{
    CFDictionaryRef props;
    Boolean         ret = FALSE;  // pre-set to failure
    CFStringRef     key = CFSTR(kIOPropertyMatchKey);
    CFDictionaryRef value;
    CFStringRef     phandleKey = CFSTR("AAPL,phandle");
    CFDataRef		data;

    data = CFDataCreate(NULL, (void *)&phandle, sizeof(UInt32));

    props = CFDictionaryCreate( NULL,
                                (void *)&phandleKey,
                                (void *)&data,
                                1,	
                                &kCFCopyStringDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks );

    value = CFDictionaryCreate( NULL,
                                (void *)&key,
                                (void *)&props,
                                1,	
                                &kCFCopyStringDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks );

   /* This call consumes 'value', so do not release it.
    */
    *device = IOServiceGetMatchingService(kIOMasterPortDefault, value);

    if (*device)
        ret = TRUE;

    CFRelease(props);
    CFRelease(data);

    return(ret);
}

static void printInterruptMap(CFTypeRef value, struct context * context)
{
    io_registry_entry_t		intParent;
    io_string_t             path;
    SInt32					childCells, parentCells;
    UInt32					*position, *end;
    CFIndex						length, count, index;
    
    // Get #address-cells and #interrupt-cells for owner
	childCells = getRecursivePropValue( context->service, CFSTR("#address-cells"   ) )
			   + getRecursivePropValue( context->service, CFSTR("#interrupt-cells" ) );

    // Walk through each table entry.
    position = (UInt32 *)CFDataGetBytePtr((CFDataRef)value);
    length = CFDataGetLength((CFDataRef)value)/sizeof(UInt32);
    end = position + length;
    count = 0;

    println("");

    while (position < end)
    {
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        print("    %02ld: ", (unsigned long)count);
        
        // Display the child's unit interrupt specifier.
        print("  child: ");
        for (index = 0; index < childCells; index++) {
            print("%08lx ", (unsigned long)*position++);
        }
        println("");

        // Lookup the phandle and retreive needed info.
        assertion( lookupPHandle(*position, &intParent), "error looking up phandle", intParent = MACH_PORT_NULL );

		parentCells = getRecursivePropValue( intParent, CFSTR( "#address-cells"   ) )
					+ getRecursivePropValue( intParent, CFSTR( "#interrupt-cells" ) );

        *path = '\0';
        makepath(intParent, path);

        IOObjectRelease(intParent);
        
        // Display the phandle, corresponding device path, and
        // the parent interrupt specifier.
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        println("        phandle: %08lx (%s)", (unsigned long)*position++, path);
        
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        print("         parent: ");
        for (index = 0; index < parentCells; index++) {
            print("%08lx ", (unsigned long)*position++);
        }
        println("");

        count++;
    }
}

static void printInterrupts(CFTypeRef value, struct context * context)
{
    UInt32					*position, *end;
    CFIndex					length, count, index;

    // Walk through each table entry.
    position = (UInt32 *)CFDataGetBytePtr((CFDataRef)value);
    length   = CFDataGetLength((CFDataRef)value) / sizeof(UInt32);
    end      = position + length;
    count    = 0;
	index    = 0;

    println("");

    while (position < end)
    {
        indent(FALSE, context->serviceDepth, context->stackOfBits);
        print("    %02ld: ", (unsigned long)index);
        
		if ( count < (length-1) )
		{
			print("specifier: %08lx (vector: %02lx) sense: %08lx (",
                (unsigned long)*position,
                (unsigned long)((*position) & 0x000000FF),
                (unsigned long)*(position+1) );
			position ++;
			count    ++;
			if ( (*position & 0x00000002 ) )	// HyperTransport
			{
				print( "HyperTransport vector: %04lx, ",
                    (unsigned long)((*position >> 16) & 0x0000FFFF));
			}

			println( "%s)", (*position & 1)? "level" : "edge" );
		}
		else
		{
			println("parent interrupt-map entry: %08lx",
                (unsigned long)*position );
		}

		position ++;
        count ++;
		index ++;
    }
}

static void printInterruptParent( CFTypeRef value, struct context * context )
{
io_registry_entry_t		parentRegEntry;
io_string_t             path;
UInt32					* pHandleValue = (UInt32 *) CFDataGetBytePtr( (CFDataRef) value );

	if ( lookupPHandle( *pHandleValue, &parentRegEntry ) )
	{
        *path = '\0';
		makepath( parentRegEntry, path );

		print( "<%08lx>", (unsigned long)*pHandleValue );
		if ( *path != '\0' )
			print( " (%s)", path );
		println( "" );

		IOObjectRelease( parentRegEntry );
	}
}

static char ToAscii(UInt32 nibble)
{
    nibble &= 0x0F;

    if (nibble <= 9)
        return((char)nibble + '0');
    else
        return((char)nibble - 10 + 'A');
}

static void printData(CFTypeRef value, struct context * context)
{
    UInt32        asciiNormalCount = 0;
    UInt32        asciiSymbolCount = 0;
    const UInt8 * bytes;
    CFIndex       index;
    CFIndex       length;

    length = CFDataGetLength(value);
    bytes  = CFDataGetBytePtr(value);

    //
    // This algorithm detects ascii strings, or a set of ascii strings, inside a
    // stream of bytes.  The string, or last string if in a set, needn't be null
    // terminated.  High-order symbol characters are accepted, unless they occur
    // too often (80% of characters must be normal).  Zero padding at the end of
    // the string(s) is valid.  If the data stream is only one byte, it is never
    // considered to be a string.
    //

    for (index = 0; index < length; index++)  // (scan for ascii string/strings)
    {
        if (bytes[index] == 0)       // (detected null in place of a new string,
        {                            //  ensure remainder of the string is null)
            for (; index < length && bytes[index] == 0; index++) { }

            break;          // (either end of data or a non-null byte in stream)
        }
        else                         // (scan along this potential ascii string)
        {
            for (; index < length; index++)
            {
                if (isprint(bytes[index]))
                    asciiNormalCount++;
                else if (bytes[index] >= 128 && bytes[index] <= 254)
                    asciiSymbolCount++;
                else
                    break;
            }

            if (index < length && bytes[index] == 0)          // (end of string)
                continue;
            else             // (either end of data or an unprintable character)
                break;
        }
    }

    if ((asciiNormalCount >> 2) < asciiSymbolCount)    // (is 80% normal ascii?)
        index = 0;
    else if (length == 1)                                 // (is just one byte?)
        index = 0;
    else if (cfshowhex)
        index = 0;

    if (index >= length && asciiNormalCount) // (is a string or set of strings?)
    {
        Boolean quoted = FALSE;

        print("<");
        
        for (index = 0; index < length; index++)
        {
            if (bytes[index])
            {
                if (quoted == FALSE)
                {
                    quoted = TRUE;
                    if (index)
                        print(",\"");
                    else
                        print("\"");
                }
                print("%c", bytes[index]);
            }
            else
            {
                if (quoted == TRUE)
                {
                    quoted = FALSE;
                    print("\"");
                }
                else
                    break;
            }
        }
        if (quoted == TRUE)
            print("\"");
            
        print(">");
    }
        
    else if (length > 8)                        // (is not a string or set of strings)
    {
        SInt8  work[ 256 ];
        SInt8* p;
        UInt32    i;
        UInt32 offset;
        CFIndex totalBytes;
        CFIndex nBytesToDraw;
        UInt32 bytesPerLine;
        UInt8  c;

        totalBytes = length;	// assume length is greater than zero

        // Calculate number of bytes per line to print, use as much screen
        // as possible.  The numbers used are derived by counting the number
        // of characters that are always printed (data address offset, white
        // space, etc ~= 20), indentation from the tree structure (2*depth)
        // and 4 characters printed per byte (two hex digits, one space, and
        // one ascii char).
        
        bytesPerLine = (context->options.width - 20 - (2*context->serviceDepth))/4;
        
        // Make sure we don't overflow the work buffer (256 bytes)
        bytesPerLine = bytesPerLine > 32 ? 32 : bytesPerLine;

        for ( offset = 0; offset < totalBytes; offset += bytesPerLine )
        {
            UInt32 offsetCopy;
            UInt16 text;

            println("");

            if ( ( offset + bytesPerLine ) <= totalBytes )
                nBytesToDraw = bytesPerLine;
            else
                nBytesToDraw = totalBytes - offset;

            offsetCopy = offset;

            // Convert offset to ASCII.
            work[ 8 ] = ':';
            p = &work[ 7 ];

            while ( offsetCopy != 0 )
            {
                *p-- = ToAscii( offsetCopy & 0x0F );
                offsetCopy >>= 4;
            }

            // Insert leading zeros.
            while ( p >= work )
                *p-- = '0';

            // Add kBytesPerLine bytes of data.
            p = &work[ 9 ];
            for ( i = 0; i < nBytesToDraw; i++ )
            {
                c = bytes[ offset + i ];
                *p++ = ' ';
                *p++ = ToAscii( ( c & 0xF0 ) >> 4 );
                *p++ = ToAscii( c & 0x0F );
            }

            // Add padding spaces.
            for ( ; i < bytesPerLine; i++ )
            {
                text = ( ( ' ' << 8 ) | ' ' );
                *( UInt16 * ) p = text;
                p[ 2 ] = ' ';
                p += 3;
            }

            *p++ = ' ';

            // Insert ASCII representation of data.
            for ( i = 0; i < nBytesToDraw; i++ )
            {
                c = bytes[ offset + i ];
                if ( ( c < ' ' ) || ( c > '~' ) )
                    c = '.';

                *p++ = c;
            }

            *p = 0;
            
            // Print this line.
            indent(FALSE, context->serviceDepth, context->stackOfBits);
            print("    %s", work);
            
        } // for
        
    } // else if (length > 32)
    else
    {
        print("<");
        for (index = 0; index < length; index++)  print("%02x", bytes[index]);
        print(">");
    }

    println("");
}
