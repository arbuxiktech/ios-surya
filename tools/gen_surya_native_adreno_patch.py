#!/usr/bin/env python3
"""Generate xnu/patches/0094-surya-unixnu-native-adreno.patch."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def new_file(name, data):
    data = data.replace(b'\r\n', b'\n')
    if b'\r' in data or not data.endswith(b'\n'):
        raise ValueError('kernel source must use LF or CRLF with final newline')
    digest = hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()
    return (f'diff --git a/iokit/Kernel/arm/{name} b/iokit/Kernel/arm/{name}\n'
            f'new file mode 100644\nindex 0000000..{digest[:7]}\n'
            f'--- /dev/null\n+++ b/iokit/Kernel/arm/{name}\n'
            f'@@ -0,0 +1,{data.count(bytes([10]))} @@\n' +
            ''.join('+' + line for line in data.decode('utf-8').splitlines(keepends=True)))


def generate():
    h_src = (ROOT / 'graphics/native/surya_native_adreno.h').read_bytes()
    cpp_src = (ROOT / 'graphics/native/surya_native_adreno.cpp').read_bytes()
    abi_src = (ROOT / 'graphics/native/SuryaNativeAdrenoABI.h').read_bytes()

    patch = new_file('SuryaNativeAdreno.h', h_src)
    patch += new_file('SuryaNativeAdreno.cpp', cpp_src)
    patch += new_file('SuryaNativeAdrenoABI.h', abi_src)

    # 1. iokit/conf/files.arm64
    patch += '''diff --git a/iokit/conf/files.arm64 b/iokit/conf/files.arm64
--- a/iokit/conf/files.arm64
+++ b/iokit/conf/files.arm64
@@ -15,6 +15,7 @@
 iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp
 iokit/Kernel/arm/SweetSurface.cpp                                optional iokitcpp
 iokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp
+iokit/Kernel/arm/SuryaNativeAdreno.cpp                           optional iokitcpp
 iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp
 iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp
 iokit/Kernel/arm/hid/IOHIDEvent.cpp                              optional iokitcpp
'''

    # 2. iokit/KernelConfigTables.cpp
    patch += '''diff --git a/iokit/KernelConfigTables.cpp b/iokit/KernelConfigTables.cpp
--- a/iokit/KernelConfigTables.cpp
+++ b/iokit/KernelConfigTables.cpp
@@ -71,6 +71,12 @@
     "     \'IOProviderClass\' = IOPlatformDevice;"
     "     \'IONameMatch\'     = \'sweet,display\';"
     "   },"
+    "   {"
+    "     \'IOClass\'         = AGXAccelerator;"
+    "     \'IOProviderClass\' = IOPlatformDevice;"
+    "     \'IONameMatch\'     = \'qcom,adreno\';"
+    "     \'IOMatchCategory\' = IOAccelerator;"
+    "   },"
     "   {"
     "     \'IOClass\'         = SweetHIDButtons;"
     "     \'IOProviderClass\' = IOResources;"
'''

    # 3. iokit/Kernel/IOUserClient.cpp (sandbox grant for backboardd / render server)
    patch += '''diff --git a/iokit/Kernel/IOUserClient.cpp b/iokit/Kernel/IOUserClient.cpp
--- a/iokit/Kernel/IOUserClient.cpp
+++ b/iokit/Kernel/IOUserClient.cpp
@@ -4325,6 +4325,23 @@
 }
 
 /* Routine io_service_open_ndr */
+/*
+ * The native GPU driver names, by the property NativeSandboxGrantProcess on its accelerator service, the one process (the render server) to which it
+ * grants its own user client in place of the sandbox's verdict: the original profiles name no class of that driver. Nothing else is affected.
+ */
+extern "C" void proc_selfname(char * buf, int size);
+static bool
+native_sandbox_grant(IOService * service)
+{
+	OSString * process = OSDynamicCast(OSString, service->getProperty("NativeSandboxGrantProcess"));
+	char name[17] = {};
+	if (!process) {
+		return false;
+	}
+	proc_selfname(name, sizeof(name));
+	return 0 == strncmp(name, process->getCStringNoCopy(), sizeof(name) - 1);
+}
+
 kern_return_t
 is_io_service_open_extended(
 	io_object_t _service,
@@ -4353,7 +4370,7 @@
 	}
 
 #if CONFIG_MACF
-	if (mac_iokit_check_open_service(kauth_cred_get(), service, connect_type) != 0) {
+	if (!native_sandbox_grant(service) && mac_iokit_check_open_service(kauth_cred_get(), service, connect_type) != 0) {
 		return kIOReturnNotPermitted;
 	}
 #endif
@@ -4512,7 +4529,9 @@
 				res = kIOReturnNotPrivileged;
 			}
 #if CONFIG_MACF
-			else if (0 != mac_iokit_check_open(kauth_cred_get(), client, connect_type)) {
+			/* The service's grant (native_sandbox_grant) stands in for the sandbox's verdict on the render server's open. */
+			else if (!native_sandbox_grant(service)
+			    && 0 != mac_iokit_check_open(kauth_cred_get(), client, connect_type)) {
 				IOLog("IOUC %s failed MACF in process %s\\n",
 				    client->getMetaClass()->getClassName(), creatorNameCStr);
 				res = kIOReturnNotPermitted;
@@ -4520,6 +4539,7 @@
 #endif
 
 			if ((kIOReturnSuccess == res)
+			    && !native_sandbox_grant(service)
 			    && gIOUCFilterCallbacks
 			    && gIOUCFilterCallbacks->io_filter_resolver) {
 				io_filter_policy_t filterPolicy;
'''

    return patch


if __name__ == '__main__':
    target = ROOT / 'xnu/patches/0094-surya-unixnu-native-adreno.patch'
    target.write_bytes(generate().encode('utf-8'))
    print(f'Wrote {target.name} ({target.stat().st_size} bytes)')
