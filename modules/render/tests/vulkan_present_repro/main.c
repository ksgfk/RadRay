// Build and reproduction instructions: docs/guide/build-test.md
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                                                 \
    do {                                                                            \
        VkResult result_ = (call);                                                  \
        if (result_ != VK_SUCCESS) {                                                \
            fprintf(stderr, "%s failed: %d (line %d)\n", #call, result_, __LINE__); \
            exit(2);                                                                \
        }                                                                           \
    } while (0)
#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "Requirement failed: %s (line %d)\n", #condition, __LINE__); \
            exit(2);                                                                     \
        }                                                                                \
    } while (0)

typedef struct Diagnostics {
    LONG errors;
    LONG presentHazards;
    unsigned round;
    const char* phase;
} Diagnostics;

typedef struct Window {
    HWND handle;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    uint32_t count;
    VkImage images[16];
    VkSemaphore present[16];
    VkBool32 initialized[16];
    VkSemaphore acquire;
    VkCommandBuffer command;
    uint32_t index;
} Window;

static VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT types,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    Diagnostics* diagnostics = user;
    (void)types;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        InterlockedIncrement(&diagnostics->errors);
        if (data->pMessageIdName && strstr(data->pMessageIdName, "WRITE-AFTER-PRESENT")) {
            InterlockedIncrement(&diagnostics->presentHazards);
        }
    }
    fprintf(stderr, "[round=%u phase=%s] %s\n", diagnostics->round, diagnostics->phase, data->pMessage);
    return VK_FALSE;
}

static void Submit(VkQueue queue, const VkSubmitInfo* info, int emptyPredecessor) {
    VkSubmitInfo batches[2] = {{0}, {0}};
    batches[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    batches[1] = *info;
    CHECK(vkQueueSubmit(queue, emptyPredecessor ? 2 : 1, emptyPredecessor ? batches : info, VK_NULL_HANDLE));
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int emptyPredecessor = 0;
    int timelineChain = 1;
    unsigned windowCount = 3;
    unsigned rounds = 12;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--empty-predecessor"))
            emptyPredecessor = 1;
        else if (!strcmp(argv[arg], "--no-timeline-chain"))
            timelineChain = 0;
        else if (!strcmp(argv[arg], "--one-window"))
            windowCount = 1;
        else if (!strcmp(argv[arg], "--rounds") && arg + 1 < argc) {
            char* end;
            unsigned long value = strtoul(argv[++arg], &end, 10);
            REQUIRE(*end == '\0' && value > 0 && value <= 10000);
            rounds = (unsigned)value;
        } else {
            fprintf(stderr, "Usage: %s [--empty-predecessor] [--no-timeline-chain] [--one-window] [--rounds N]\n", argv[0]);
            return 2;
        }
    }
    Diagnostics diagnostics = {0, 0, 0, "setup"};
    printf("windows=%u rounds=%u timeline_chain=%d empty_predecessor=%d\n", windowCount, rounds, timelineChain, emptyPredecessor);
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkDebugUtilsMessengerCreateInfoEXT debug = {0};
    debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = OnMessage;
    debug.pUserData = &diagnostics;
    VkValidationFeaturesEXT validation = {0};
    validation.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validation.pNext = &debug;
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &enabled;
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Native Vulkan present reproduction";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo = {0};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = &validation;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = &layer;
    instanceInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    instanceInfo.ppEnabledExtensionNames = extensions;
    VkInstance instance;
    CHECK(vkCreateInstance(&instanceInfo, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT createDebug = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroyDebug = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    REQUIRE(createDebug && destroyDebug);
    VkDebugUtilsMessengerEXT messenger;
    CHECK(createDebug(instance, &debug, NULL, &messenger));
    HMODULE layerModule = GetModuleHandleA("VkLayer_khronos_validation.dll");
    char layerPath[MAX_PATH] = {0};
    REQUIRE(layerModule && GetModuleFileNameA(layerModule, layerPath, MAX_PATH));
    printf("Validation DLL: %s\n", layerPath);

    HINSTANCE module = GetModuleHandleW(NULL);
    WNDCLASSW windowClass = {0};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = module;
    windowClass.lpszClassName = L"NativeVulkanPresentRepro";
    REQUIRE(RegisterClassW(&windowClass));
    Window windows[3] = {0};
    for (unsigned i = 0; i < windowCount; ++i) {
        Window* w = &windows[i];
        w->handle = CreateWindowExW(0, windowClass.lpszClassName, L"Native Vulkan present repro", WS_OVERLAPPEDWINDOW,
                                    80 + (int)i * 270, 80, 256, 224, NULL, NULL, module, NULL);
        REQUIRE(w->handle);
        ShowWindow(w->handle, SW_SHOWNOACTIVATE);
        VkWin32SurfaceCreateInfoKHR surfaceInfo = {0};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.hinstance = module;
        surfaceInfo.hwnd = w->handle;
        CHECK(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, NULL, &w->surface));
    }

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    uint32_t physicalCount = 32;
    VkPhysicalDevice physicalDevices[32];
    CHECK(vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices));
    for (uint32_t p = 0; p < physicalCount && !physical; ++p) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicalDevices[p], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2) continue;
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature = {0};
        timelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        VkPhysicalDeviceFeatures2 features = {0};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &timelineFeature;
        vkGetPhysicalDeviceFeatures2(physicalDevices[p], &features);
        if (!timelineFeature.timelineSemaphore) continue;
        uint32_t count = 64;
        VkQueueFamilyProperties families[64];
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[p], &count, families);
        for (uint32_t f = 0; f < count; ++f) {
            if (!(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 supported = VK_TRUE;
            for (unsigned i = 0; i < windowCount; ++i) {
                VkBool32 present;
                CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[p], f, windows[i].surface, &present));
                supported &= present;
            }
            if (supported) {
                physical = physicalDevices[p];
                family = f;
                printf("GPU: %s API=%u.%u.%u\n", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
                break;
            }
        }
    }
    REQUIRE(physical);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {0};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature = {0};
    timelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeature.timelineSemaphore = VK_TRUE;
    const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo = {0};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &timelineFeature;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &deviceExtension;
    VkDevice device;
    CHECK(vkCreateDevice(physical, &deviceInfo, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &poolInfo, NULL, &pool));
    VkSemaphoreCreateInfo semaphoreInfo = {0};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (unsigned i = 0; i < windowCount; ++i) {
        Window* w = &windows[i];
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, w->surface, &caps));
        REQUIRE(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        uint32_t formatCount = 64;
        VkSurfaceFormatKHR formats[64];
        CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, w->surface, &formatCount, formats));
        REQUIRE(formatCount);
        VkSurfaceFormatKHR format = formats[0];
        for (uint32_t f = 0; f < formatCount; ++f) {
            if (formats[f].format == VK_FORMAT_B8G8R8A8_UNORM) format = formats[f];
        }
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
        VkSwapchainCreateInfoKHR swapchainInfo = {0};
        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = w->surface;
        swapchainInfo.minImageCount = caps.minImageCount > 3 ? caps.minImageCount : 3;
        if (caps.maxImageCount && swapchainInfo.minImageCount > caps.maxImageCount) swapchainInfo.minImageCount = caps.maxImageCount;
        swapchainInfo.imageFormat = format.format;
        swapchainInfo.imageColorSpace = format.colorSpace;
        swapchainInfo.imageExtent = caps.currentExtent;
        if (caps.currentExtent.width == UINT32_MAX) {
            swapchainInfo.imageExtent = caps.minImageExtent;
        }
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = caps.currentTransform;
        swapchainInfo.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;
        CHECK(vkCreateSwapchainKHR(device, &swapchainInfo, NULL, &w->swapchain));
        w->count = 16;
        CHECK(vkGetSwapchainImagesKHR(device, w->swapchain, &w->count, w->images));
        for (uint32_t j = 0; j < w->count; ++j) CHECK(vkCreateSemaphore(device, &semaphoreInfo, NULL, &w->present[j]));
        CHECK(vkCreateSemaphore(device, &semaphoreInfo, NULL, &w->acquire));
        VkCommandBufferAllocateInfo allocate = {0};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(device, &allocate, &w->command));
    }
    VkSemaphoreTypeCreateInfo typeInfo = {0};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreInfo.pNext = &typeInfo;
    VkSemaphore timeline;
    CHECK(vkCreateSemaphore(device, &semaphoreInfo, NULL, &timeline));
    uint64_t value = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        diagnostics.round = round;
        diagnostics.phase = "acquire";
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        for (unsigned i = 0; i < windowCount; ++i) {
            Window* w = &windows[i];
            // The previous round's host wait proves this acquire semaphore was consumed.
            VkResult acquired = vkAcquireNextImageKHR(device, w->swapchain, UINT64_MAX, w->acquire, VK_NULL_HANDLE, &w->index);
            REQUIRE(acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR);
        }
        diagnostics.phase = "submit";
        for (unsigned step = 0; step < windowCount; ++step) {
            unsigned i = round % 2 ? windowCount - 1 - step : step;
            Window* w = &windows[i];
            CHECK(vkResetCommandBuffer(w->command, 0));
            VkCommandBufferBeginInfo begin = {0};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            CHECK(vkBeginCommandBuffer(w->command, &begin));
            VkImageMemoryBarrier barrier = {0};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = w->initialized[w->index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = w->images[w->index];
            barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            // ALL_COMMANDS for both acquire wait and barriers avoids narrow stage-scope questions.
            vkCmdPipelineBarrier(w->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
            VkClearColorValue color = {{0.1f, 0.2f, 0.3f, 1.0f}};
            vkCmdClearColorImage(w->command, barrier.image, barrier.newLayout, &color, 1, &barrier.subresourceRange);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(w->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
            CHECK(vkEndCommandBuffer(w->command));
            w->initialized[w->index] = VK_TRUE;
            VkSemaphore waits[] = {w->acquire, timeline};
            VkSemaphore signals[] = {w->present[w->index], timeline};
            uint64_t waitValues[] = {0, value};
            uint64_t signalValues[] = {0, ++value};
            VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            VkTimelineSemaphoreSubmitInfo timelineInfo = {0};
            timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineInfo.waitSemaphoreValueCount = timelineChain ? 2 : 1;
            timelineInfo.pWaitSemaphoreValues = waitValues;
            timelineInfo.signalSemaphoreValueCount = 2;
            timelineInfo.pSignalSemaphoreValues = signalValues;
            VkSubmitInfo submit = {0};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.pNext = &timelineInfo;
            submit.waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount;
            submit.pWaitSemaphores = waits;
            submit.pWaitDstStageMask = stages;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &w->command;
            submit.signalSemaphoreCount = 2;
            submit.pSignalSemaphores = signals;
            Submit(queue, &submit, emptyPredecessor);
        }
        uint64_t previous = value++;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timelineInfo = {0};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.waitSemaphoreValueCount = timelineChain ? 1 : 0;
        timelineInfo.pWaitSemaphoreValues = &previous;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &value;
        VkSubmitInfo tail = {0};
        tail.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        tail.pNext = &timelineInfo;
        tail.waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount;
        tail.pWaitSemaphores = &timeline;
        tail.pWaitDstStageMask = &stage;
        tail.signalSemaphoreCount = 1;
        tail.pSignalSemaphores = &timeline;
        Submit(queue, &tail, emptyPredecessor);
        diagnostics.phase = "host-wait";
        VkSemaphoreWaitInfo hostWait = {0};
        hostWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        hostWait.semaphoreCount = 1;
        hostWait.pSemaphores = &timeline;
        hostWait.pValues = &value;
        CHECK(vkWaitSemaphores(device, &hostWait, UINT64_MAX));
        diagnostics.phase = "present";
        for (unsigned step = 0; step < windowCount; ++step) {
            unsigned i = round % 2 ? windowCount - 1 - step : step;
            Window* w = &windows[i];
            VkPresentInfoKHR present = {0};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &w->present[w->index];
            present.swapchainCount = 1;
            present.pSwapchains = &w->swapchain;
            present.pImageIndices = &w->index;
            VkResult result = vkQueuePresentKHR(queue, &present);
            REQUIRE(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
        }
    }
    printf("LOOP errors=%ld WRITE_AFTER_PRESENT=%ld\n", diagnostics.errors, diagnostics.presentHazards);
    diagnostics.phase = "cleanup";
    // Conventional unextended WSI teardown; loop diagnostics are reported separately.
    CHECK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySemaphore(device, timeline, NULL);
    for (unsigned i = 0; i < windowCount; ++i) {
        Window* w = &windows[i];
        for (uint32_t j = 0; j < w->count; ++j) vkDestroySemaphore(device, w->present[j], NULL);
        vkDestroySemaphore(device, w->acquire, NULL);
        vkDestroySwapchainKHR(device, w->swapchain, NULL);
        vkDestroySurfaceKHR(instance, w->surface, NULL);
        DestroyWindow(w->handle);
    }
    vkDestroyDevice(device, NULL);
    destroyDebug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    UnregisterClassW(windowClass.lpszClassName, module);
    printf("TOTAL errors=%ld WRITE_AFTER_PRESENT=%ld\n", diagnostics.errors, diagnostics.presentHazards);
    return diagnostics.errors ? 1 : 0;
}
