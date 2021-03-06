#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <stdexcept>
#include <set>
#include <algorithm>
#include <fstream>
#include <string>

#include "graphicsEngine.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

GraphicsEngine::GraphicsEngine(int width, int height, std::string) : width(width), height(height), title(title) {}

void GraphicsEngine::init() {
	initWindow();
	initVulkan();
}

uint32_t GraphicsEngine::getWidth() {
	return swapChainExtent.width;
}

uint32_t GraphicsEngine::getHeight() {
	return swapChainExtent.height;
}

bool GraphicsEngine::updateRectList(std::vector<Rect> &rects) {
	if (rects.size() == 0) {
		std::cerr << "Tried to add empty vector of rects to engine" << std::endl;
		return false;
	}

	bool init = rectCount == 0;
		rectCount = 0;

	// Temporarily allocate memory for model matrices in all materials which is reallocated to the correct size later
	for (Material& material : materials) {
		// Clear and reserve space for vertices and indices, resets count and free allocated memory
		material.reset(rects.size());

		material.modelMatrices = (glm::mat4*)malloc(sizeof(glm::mat4) * rects.size());
	}

	for (Rect& rect : rects) {
		float width = rect.width;
		float height = rect.height;
		int textureIndex = rect.textureIndex;

		Material& material = materials[textureIndex];

		material.vertices.push_back({ {0.0f , 0.0f  }, { 1.0f, 0.0f, 0.0f }, {0.0f, 0.0f} });
		material.vertices.push_back({ {width, 0.0f  }, { 1.0f, 0.0f, 0.0f }, {1.0f, 0.0f} });
		material.vertices.push_back({ {width, height}, { 1.0f, 0.0f, 0.0f }, {1.0f, 1.0f} });
		material.vertices.push_back({ {0.0f , height}, { 1.0f, 0.0f, 0.0f }, {0.0f, 1.0f} });


		uint32_t vertexCount = (uint32_t)material.vertices.size() - 4;
		material.indices.push_back(vertexCount);
		material.indices.push_back(vertexCount + 1);
		material.indices.push_back(vertexCount + 2);
		material.indices.push_back(vertexCount + 2);
		material.indices.push_back(vertexCount + 3);
		material.indices.push_back(vertexCount);


		material.modelMatrices[material.count] = glm::mat4(1.0f);
		rect.modelMatrix = &material.modelMatrices[material.count];

		material.count++;
		rectCount++;
	}


	// Correcting the size of the model matrices memory
	/*for (Material &mat : materials) {
		void* tmpPtr = realloc(mat.modelMatrices, sizeof(glm::mat4) * mat.count);
		if (tmpPtr) {
			mat.modelMatrices = (glm::mat4*)tmpPtr;
		}
		else {
			throw std::runtime_error("Failed to reallocate memory for model matrices");
		}
	}*/

	createGameObjectData(init);

	return true;
}

void GraphicsEngine::createGameObjectData(bool init) {
	if (!init) {
		// Wait for VkBuffers and command pool to stop pending
		vkDeviceWaitIdle(device);

		for (size_t i = 0; i < modelBuffers.size(); i++) {
			vkUnmapMemory(device, modelBufferMemory[i]);
			vkDestroyBuffer(device, modelBuffers[i], nullptr);
			vkFreeMemory(device, modelBufferMemory[i], nullptr);
		}

		vkDestroyDescriptorPool(device, descriptorPool, nullptr);

		// Reset command pool much faster than free and reallocate command buffer and little faster than reset command buffer
		vkResetCommandPool(device, commandPool, 0); //VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT??

		for (Material& material : materials) {
			vkDestroyBuffer(device, material.indexBuffer, nullptr);
			vkFreeMemory(device, material.indexBufferMemory, nullptr);

			vkDestroyBuffer(device, material.vertexBuffer, nullptr);
			vkFreeMemory(device, material.vertexBufferMemory, nullptr);
		}
	}

	createVertexBuffer();
	createIndexBuffer();
	
	setupDescriptors();

	recordCommandBuffer();
}

void GraphicsEngine::initWindow() {
	glfwInit();

	// Don't create a OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
}

void GraphicsEngine::framebufferResizeCallback(GLFWwindow *window, int width, int height) {
	GraphicsEngine *app = reinterpret_cast<GraphicsEngine*>(glfwGetWindowUserPointer(window));
	app->frameBufferResized = true;
}

void GraphicsEngine::initVulkan() {
	// Setup Vulkan instance
	createInstance();

	setupDebugMessenger();

	// Get window
	createSurface();

	// Pick GPU
	pickPhysicalDevice();

	createLogicalDevice();
	createSwapChain();

	createProjViewUBO();
	
	shaderBufferNeedsUpdate = std::vector<bool>(swapChainImages.size(), false);

	createImageViews();
	createRenderPass();
	createDescriptorSetLayout();
	createGraphicsPipeline();
	createFramebuffers();
	createCommandPool();

	//getAlignments();

	createCommandBuffer();

	// Create semaphores and fences
	createSyncObjects();

	createTextureSampler();
}

void GraphicsEngine::drawFrame() {
	vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);		

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
		
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		recreateSwapChain();
		return;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("Failed to reobtain swap chain image");
	}


	if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
		vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
	}
	imagesInFlight[imageIndex] = inFlightFences[currentFrame];

	// Update the model matrices of the objects
	int offset = 0;
	for (size_t i = 0; i < materials.size(); i++) {
		for (size_t j = 0; j < materials[i].count; j++) {
			memcpy(mappedDeviceMemPtrs[imageIndex * rectCount + offset + j], &materials[i].modelMatrices[j], sizeof(glm::mat4));
		}
		offset += materials[i].count;
	}


	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] }; // Each semaphore corresponds to a specific stage in the pipeline
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffers[imageIndex];

	VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	vkResetFences(device, 1, &inFlightFences[currentFrame]);

	if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
		throw std::runtime_error("Failed to submit draw command buffer");
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = { swapChain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;

	result = vkQueuePresentKHR(presentQueue, &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || frameBufferResized) {
		frameBufferResized = false;
		recreateSwapChain();
	}
	else if (result != VK_SUCCESS) {
		throw std::runtime_error("Failed to present swap chain image");
	}

	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

bool GraphicsEngine::shouldClose() {
	return glfwWindowShouldClose(window);
}

void GraphicsEngine::changeTitle(std::string fpsString) {
	fpsString += std::to_string(rectCount) + " rectangles";
	glfwSetWindowTitle(window, fpsString.c_str());
}

void GraphicsEngine::pollEvents() {
	glfwPollEvents();
}

bool GraphicsEngine::checkMouseClick() {
	int state = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT);
	return (state == GLFW_PRESS);
}

bool GraphicsEngine::checkKeyPress(int key) {
	int state = glfwGetKey(window, key);
	return (state == GLFW_PRESS);
}

void GraphicsEngine::cleanup() {
	vkDeviceWaitIdle(device);

	cleanupSwapChain();

	for (size_t i = 0; i < modelBuffers.size(); i++) {
		vkUnmapMemory(device, modelBufferMemory[i]);
		vkDestroyBuffer(device, modelBuffers[i], nullptr);
		vkFreeMemory(device, modelBufferMemory[i], nullptr);
	}

	for (size_t i = 0; i < textureImageViews.size(); i++) {
		vkDestroyImageView(device, textureImageViews[i], nullptr);
		vkDestroyImage(device, textureImages[i], nullptr);
	}

	vkFreeMemory(device, textureImageMemory, nullptr);
	vkDestroySampler(device, textureSampler, nullptr);

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		vkDestroyBuffer(device, projViewBuffers[i], nullptr);
		vkFreeMemory(device, projViewBufferMemory[i], nullptr);
	}

	vkDestroyDescriptorPool(device, descriptorPool, nullptr);

	for (size_t i = 0; i < descriptorSetLayout.size(); i++) {
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout[i], nullptr);
	}

	vkFreeCommandBuffers(device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
	vkDestroyCommandPool(device, commandPool, nullptr);

	for (Material const &material : materials) {
		vkDestroyBuffer(device, material.indexBuffer, nullptr);
		vkFreeMemory(device, material.indexBufferMemory, nullptr);

		vkDestroyBuffer(device, material.vertexBuffer, nullptr);
		vkFreeMemory(device, material.vertexBufferMemory, nullptr);
	}


	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
		vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
		vkDestroyFence(device, inFlightFences[i], nullptr);
	}

	vkDestroyDevice(device, nullptr);

	if (enableValidationLayers) {
		DestroyDebugUtilsMessengerEXT(instance, nullptr, debugMessenger);
	}

	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyInstance(instance, nullptr);

	glfwDestroyWindow(window);
	glfwTerminate();

	// Free model matrices
}

void GraphicsEngine::cleanupSwapChain() {
	for (VkFramebuffer &framebuffer : swapChainFramebuffers) {
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	}

	vkDestroyPipeline(device, graphicsPipeline, nullptr);
	vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
	vkDestroyRenderPass(device, renderPass, nullptr);

	for (VkImageView &imageView : swapChainImageViews) {
		vkDestroyImageView(device, imageView, nullptr);
	}

	vkDestroySwapchainKHR(device, swapChain, nullptr);
}

void GraphicsEngine::createInstance() {
	if (enableValidationLayers && !checkValidationLayerSupport()) {
		throw std::runtime_error("Validation layers not available");
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Vulkan Test Application";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.pEngineName = "None";
	appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	std::vector<const char *> extensions = getRequiredExtensions();

	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();

		// Doesn't work, even when deleting call to DestroyDebugUtilsMessengerEXT
		populateDebugMessengerCreateInfo(debugCreateInfo);
		createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT *)&debugCreateInfo;
		// only needed for instance creation and destruction, but would be nice to have
	}
	else {
		createInfo.enabledLayerCount = 0;
		createInfo.pNext = nullptr;
	}

	if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create a Vulkan instance");
	}
}

bool GraphicsEngine::checkValidationLayerSupport() {
	uint32_t layerCount;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

	std::vector<VkLayerProperties> availableLayers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

	for (const char *layerName : validationLayers) {
		bool layerFound = false;

		for (const VkLayerProperties &layerProperties : availableLayers) {
			if (strcmp(layerName, layerProperties.layerName) == 0) {
				layerFound = true;
				break;
			}
		}

		if (!layerFound) {
			return false;
		}
	}

	return true;
}

std::vector<const char *> GraphicsEngine::getRequiredExtensions() {
	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions;

	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

	if (enableValidationLayers) {
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	return extensions;
}

void GraphicsEngine::setupDebugMessenger() {
	if (!enableValidationLayers) return;

	VkDebugUtilsMessengerCreateInfoEXT createInfo{};
	populateDebugMessengerCreateInfo(createInfo);

	if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
		throw std::runtime_error("Failed to set up debug messenger");
	}
}

void GraphicsEngine::populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo) {
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	// Not included: VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = debugCallback;
	createInfo.pUserData = nullptr; // Pointer to additional user data to be sent to the callback function

}

VKAPI_ATTR VkBool32 VKAPI_CALL GraphicsEngine::debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
	void *pUserData)
{
	std::cerr << pCallbackData->pMessage << std::endl;

	return VK_FALSE;
}

void GraphicsEngine::createSurface() {
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create window surface");
	}
}

void GraphicsEngine::pickPhysicalDevice() {
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	if (deviceCount == 0) {
		throw std::runtime_error("Failed to find GPUs with Vulkan support");
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	for (const VkPhysicalDevice &device : devices) {
		if (isDeviceSuitable(device)) {
			physicalDevice = device;
			break;
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("Failed to find a suitable GPU");
	}
}

// Check if all used functionality is available on the GPU
bool GraphicsEngine::isDeviceSuitable(VkPhysicalDevice device) {
	QueueFamilyIndices indices = findQueueFamilies(device);

	bool extensionsSupported = checkDeviceExtensionSupport(device);

	bool swapChainSuitable = false;
	if (extensionsSupported) {
		SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
		swapChainSuitable = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	VkPhysicalDeviceFeatures supportedFeatures;
	vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

	return indices.hasValue() && extensionsSupported && swapChainSuitable && supportedFeatures.samplerAnisotropy;
}

bool GraphicsEngine::checkDeviceExtensionSupport(VkPhysicalDevice device) {
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

	for (const char *extensionName : deviceExtensions) {
		bool extensionFound = false;

		for (const VkExtensionProperties &extensionProperties : availableExtensions) {
			if (strcmp(extensionName, extensionProperties.extensionName) == 0) {
				extensionFound = true;
				break;
			}
		}

		if (!extensionFound) {
			return false;
		}
	}

	return true;
}

QueueFamilyIndices GraphicsEngine::findQueueFamilies(VkPhysicalDevice device) {
	QueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	int i = 0;
	for (const VkQueueFamilyProperties &queueFamily : queueFamilies) {
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}

		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

		if (presentSupport) {
			indices.presentFamily = i;
		}

		if (indices.hasValue()) {
			break;
		}

		i++;
	}
	return indices;
}

void GraphicsEngine::createLogicalDevice() {
	QueueFamilyIndices indices = findQueueFamilies(physicalDevice);


	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = { indices.graphicsFamily.value(), indices.presentFamily.value() };

	float queuePriority = 1.0f;
	for (uint32_t queueFamily : uniqueQueueFamilies) {
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}


	VkPhysicalDeviceFeatures deviceFeatures{};
	deviceFeatures.samplerAnisotropy = VK_TRUE;

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();

	createInfo.pEnabledFeatures = &deviceFeatures;

	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();

	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	}
	else {
		createInfo.enabledLayerCount = 0;
	}

	if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create logical device");
	}

	vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
}

void GraphicsEngine::createSwapChain() {
	SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);

	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
	swapChainExtent = chooseSwapExtent(swapChainSupport.capabilities);

	swapChainImageFormat = surfaceFormat.format;

	UBO.matrix = glm::ortho(0.0f, (float)swapChainExtent.width, 0.0f, (float)swapChainExtent.height);

	// Set number of images in the swap chain
	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = swapChainExtent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; //VK_IMAGE_USAGE_TRANSFER_DST_BIT if post-processing

	QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
	uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };

	// If graphic and present queues are different
	if (indices.graphicsFamily != indices.presentFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.queueFamilyIndexCount = 0;
		createInfo.pQueueFamilyIndices = nullptr;
	}

	createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create a swap chain");
	}

	vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);

	swapChainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());
}

SwapChainSupportDetails GraphicsEngine::querySwapChainSupport(VkPhysicalDevice device) {
	SwapChainSupportDetails details;

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

	if (formatCount != 0) {
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

	if (presentModeCount != 0) {
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
	}
	return details;
}

// Find the best surface format (Color)
VkSurfaceFormatKHR GraphicsEngine::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats) {
	for (const VkSurfaceFormatKHR &availableFormat : availableFormats) {
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}

	// Return the first if no good format is found 
	return availableFormats[0];
}

// See if triple buffering is available otherwise use VSync
VkPresentModeKHR GraphicsEngine::chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes) {
	for (const VkPresentModeKHR &availablePresentMode : availablePresentModes) {
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return availablePresentMode;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

// Choose resolution of the swap chain
VkExtent2D GraphicsEngine::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilites) {
	if (capabilites.currentExtent.width != UINT32_MAX) {
		return capabilites.currentExtent;
	}
	else {
		int width, height;
		glfwGetFramebufferSize(window, &width, &height);
		VkExtent2D actualExtent = {
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height) 
		};

		actualExtent.width = std::clamp(actualExtent.width, capabilites.minImageExtent.width, capabilites.maxImageExtent.width);
		actualExtent.height = std::clamp(actualExtent.height, capabilites.minImageExtent.height, capabilites.maxImageExtent.height);

		return actualExtent;
	}
}

void GraphicsEngine::createImageViews() {
	swapChainImageViews.resize(swapChainImages.size());

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		swapChainImageViews[i] = createImageView(swapChainImages[i], swapChainImageFormat);
	}
}

VkImageView GraphicsEngine::createImageView(VkImage image, VkFormat format) {
	VkImageViewCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	createInfo.image = image;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = format;

	createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	createInfo.subresourceRange.levelCount = 1;
	createInfo.subresourceRange.baseMipLevel = 0;
	createInfo.subresourceRange.layerCount = 1;
	createInfo.subresourceRange.baseArrayLayer = 0;

	VkImageView imageView;
	if (vkCreateImageView(device, &createInfo, nullptr, &imageView) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create image view");
	}

	return imageView;
}

void GraphicsEngine::createRenderPass() {
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = swapChainImageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;

	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // LOAD to disable clearing between frames
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Change to same as below if not clearing between frames??
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1; // Shader layout directive??
	subpass.pColorAttachments = &colorAttachmentRef;

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;

	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;

	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &colorAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create render pass");
	}
}

void GraphicsEngine::createDescriptorSetLayout() {
	VkDescriptorSetLayoutCreateInfo layoutInfo{};

	// DescriptorSetLayout for descriptors that change once per scene
	VkDescriptorSetLayoutBinding sceneLayoutBindings[2];

	// UBO for the projection and view matrix
	sceneLayoutBindings[0] = {};
	sceneLayoutBindings[0].binding = 0;
	sceneLayoutBindings[0].descriptorCount = 1;
	sceneLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sceneLayoutBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	// Sampler
	sceneLayoutBindings[1] = {};
	sceneLayoutBindings[1].binding = 1;
	sceneLayoutBindings[1].descriptorCount = 1;
	sceneLayoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	sceneLayoutBindings[1].pImmutableSamplers = nullptr;
	sceneLayoutBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = sceneLayoutBindings;

	descriptorSetLayout.push_back(VkDescriptorSetLayout());

	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout.back()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create descriptor set layout");
	}



	// DescriptorSetLayout for descriptors that change once per material
	VkDescriptorSetLayoutBinding materialLayoutBindings[2];

	// Sampled image
	materialLayoutBindings[0] = {};
	materialLayoutBindings[0].binding = 0;
	materialLayoutBindings[0].descriptorCount = 1;
	materialLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	materialLayoutBindings[0].pImmutableSamplers = nullptr;
	materialLayoutBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = materialLayoutBindings;

	descriptorSetLayout.push_back(VkDescriptorSetLayout());

	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout.back()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create descriptor set layout");
	}



	// DescriptorSetLayout for descriptors that change once per object
	VkDescriptorSetLayoutBinding objectLayoutBindings[2];

	// UBO for the model matrix
	objectLayoutBindings[0] = {};
	objectLayoutBindings[0].binding = 0;
	objectLayoutBindings[0].descriptorCount = 1;
	objectLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	objectLayoutBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = objectLayoutBindings;

	descriptorSetLayout.push_back(VkDescriptorSetLayout());

	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout.back()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create descriptor set layout");
	}
}

void GraphicsEngine::createGraphicsPipeline() {
	std::vector<char> vertexShaderCode = readFile("shaders/vert.spv");
	std::vector<char> fragmentShaderCode = readFile("shaders/frag.spv");

	VkShaderModule vertexShaderModule = createShaderModule(vertexShaderCode);
	VkShaderModule fragmentShaderModule = createShaderModule(fragmentShaderCode);

	VkPipelineShaderStageCreateInfo vertexShaderStageInfo{};
	vertexShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertexShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertexShaderStageInfo.module = vertexShaderModule;
	vertexShaderStageInfo.pName = "main";

	VkPipelineShaderStageCreateInfo fragmentShaderStageInfo{};
	fragmentShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragmentShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragmentShaderStageInfo.module = fragmentShaderModule;
	fragmentShaderStageInfo.pName = "main";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertexShaderStageInfo, fragmentShaderStageInfo };

	auto bindingDescription = Vertex::getBindingDescription();
	auto attributeDescriptions = Vertex::getAttributeDescriptions();

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;

	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // Which type of primitive to draw
	inputAssembly.primitiveRestartEnable = VK_FALSE; // controls _STRIP topologies index

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)swapChainExtent.width;
	viewport.height = (float)swapChainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = swapChainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE; // true if you want to clamp vertices to the far and near plane for shadows
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL; // LINE for wireframes, requires gpu feature
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasClamp = 0.0f;
	rasterizer.depthBiasSlopeFactor = 0.0f;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisampling.minSampleShading = 1.0f;
	multisampling.pSampleMask = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.blendEnable = VK_TRUE;
	colorBlendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	// Alpha blending
	colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;

	colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;


	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 3; // Uniform sets (not descriptor count)
	pipelineLayoutInfo.pSetLayouts = descriptorSetLayout.data();
	pipelineLayoutInfo.pushConstantRangeCount = 0; // Push constants
	pipelineLayoutInfo.pPushConstantRanges = nullptr;

	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create a pipeline layout");
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;

	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = nullptr;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = nullptr;

	pipelineInfo.layout = pipelineLayout;

	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create graphics pipeline");
	}

	vkDestroyShaderModule(device, vertexShaderModule, nullptr);
	vkDestroyShaderModule(device, fragmentShaderModule, nullptr);
}

std::vector<char> GraphicsEngine::readFile(const std::string &fileName) {
	std::ifstream file(fileName, std::ios::ate | std::ios::binary);

	if (!file.is_open()) {
		throw std::runtime_error("Failed to open file " + fileName);
	}

	size_t fileSize = (size_t)file.tellg();
	std::vector<char> buffer(fileSize);

	file.seekg(0);
	file.read(buffer.data(), fileSize);

	file.close();

	return buffer;
}

VkShaderModule GraphicsEngine::createShaderModule(const std::vector<char> &code) {
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data()); // uint32_t needs to be aligned, but std::vector aligns data automatically

	VkShaderModule shaderModule;
	if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shader module");
	}

	return shaderModule;
}

void GraphicsEngine::createFramebuffers() {
	swapChainFramebuffers.resize(swapChainImageViews.size());

	for (size_t i = 0; i < swapChainImageViews.size(); i++) {
		VkImageView attachments[] = {
			swapChainImageViews[i]
		};

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = renderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = swapChainExtent.width;
		framebufferInfo.height = swapChainExtent.height;
		framebufferInfo.layers = 1;

		if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create framebuffer");
		}
	}
}

void GraphicsEngine::createCommandPool() {
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
	poolInfo.flags = 0; // transient_bit if rerecorded often, reset_command_buffer_bit if rerecorded individually

	if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create a command pool");
	}
}

void GraphicsEngine::createTextureImage(std::string fileName) {
	// Load texture image
	int textureWidth, textureHeight, textureChannels;
	stbi_uc* pixels = stbi_load(
		fileName.c_str(), &textureWidth, &textureHeight, &textureChannels, STBI_rgb_alpha
	);
	VkDeviceSize imageSize = static_cast<uint64_t>(textureWidth) * static_cast<uint64_t>(textureHeight) * 4;

	if (!pixels) {
		throw std::runtime_error("Failed to load texture image");
	}

	VkBuffer stagingBuffer;
	VkDeviceMemory stagingBufferMemory;

	createBuffer(
		imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		stagingBuffer, stagingBufferMemory
	);

	void* data;
	vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
	memcpy(data, pixels, static_cast<size_t>(imageSize));
	vkUnmapMemory(device, stagingBufferMemory);
	stbi_image_free(pixels);

	textureImages.push_back(VkImage());
	// Using image objects instead of accessing buffer directly from shader
	createImage(
		textureWidth, textureHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImages.back(), textureImageMemory
	);

	transitionImageLayout(textureImages.back(), VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	copyBufferToImage(stagingBuffer, textureImages.back(), static_cast<uint32_t>(textureWidth), static_cast<uint32_t>(textureHeight));
	transitionImageLayout(textureImages.back(), VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	
	vkDestroyBuffer(device, stagingBuffer, nullptr);
	vkFreeMemory(device, stagingBufferMemory, nullptr);

	VkImageView textureImageView = createImageView(textureImages.back(), VK_FORMAT_R8G8B8A8_SRGB);
	
	textureImageViews.push_back(textureImageView);
	materials.push_back(Material());
}

void GraphicsEngine::createImage(
	uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
	VkMemoryPropertyFlags properties, VkImage &image, VkDeviceMemory &imageMemory
) {
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create texture image");
	}

	VkMemoryRequirements memRequirements{};
	vkGetImageMemoryRequirements(device, image, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
		throw std::runtime_error("Failed to allocate image memory");
	}

	vkBindImageMemory(device, image, imageMemory, 0);		
}

void GraphicsEngine::transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands();

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout; // or VK_IMAGE_LAYOUT_UNDEFINED if you don't care about existing image
	barrier.newLayout = newLayout;

	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	
	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else {
		throw std::invalid_argument("Unsupported image layout transition");
	}

	vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	endSingleTimeCommands(commandBuffer);
}

void GraphicsEngine::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands();

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;

	region.imageOffset = { 0,0,0 };
	region.imageExtent = { width, height, 1 };

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	endSingleTimeCommands(commandBuffer);
}

void GraphicsEngine::createTextureSampler() {
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR; // Bilinear filtering
	samplerInfo.minFilter = VK_FILTER_LINEAR;

	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

	samplerInfo.anisotropyEnable = VK_TRUE;
	samplerInfo.maxAnisotropy = 16.0f; // Texture sampling 16x

	samplerInfo.unnormalizedCoordinates = VK_FALSE;

	samplerInfo.compareEnable = VK_FALSE; // Shadow map antialiasing
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	if (vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create a texture sampler");
	}
}

void GraphicsEngine::createVertexBuffer() {
	for (Material &material : materials) {
		if (material.count == 0) break;

		VkDeviceSize bufferSize = sizeof(Vertex) * material.vertices.size();

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(
			bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory
		);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, material.vertices.data(), (size_t)bufferSize);
		vkUnmapMemory(device, stagingBufferMemory);

		createBuffer(
			bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, material.vertexBuffer, material.vertexBufferMemory
		);

		copyBuffer(stagingBuffer, material.vertexBuffer, bufferSize);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
	}
}

void GraphicsEngine::createIndexBuffer() {
	for (Material &material : materials) {
		if (material.count == 0) break;

		VkDeviceSize bufferSize = sizeof(uint32_t) * material.indices.size();

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(
			bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory
		);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, material.indices.data(), (size_t)bufferSize);
		vkUnmapMemory(device, stagingBufferMemory);

		createBuffer(
			bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, material.indexBuffer, material.indexBufferMemory
		);

		copyBuffer(stagingBuffer, material.indexBuffer, bufferSize);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
	}
}

void GraphicsEngine::getAlignments() {
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physicalDevice, &props);

	/*std::cout << "Max dynamic ubos " << props.limits.maxDescriptorSetUniformBuffersDynamic << std::endl;
	std::cout << "Max dynamic ubo size " << props.limits.maxUniformBufferRange << std::endl;
	std::cout << "Min ubo alignment " << props.limits.minUniformBufferOffsetAlignment << "\n" << std::endl;

	std::cout << "Max storage buffer " << props.limits.maxDescriptorSetStorageBuffersDynamic << std::endl;
	std::cout << "Max storage buffer size " << props.limits.maxStorageBufferRange << std::endl;
	std::cout << "Storage buffer alignment " << props.limits.minStorageBufferOffsetAlignment << "\n" << std::endl;

	std::cout << "Max memory allocation count " << props.limits.maxMemoryAllocationCount << std::endl;
	std::cout << "Max pushconstant size " << props.limits.maxPushConstantsSize << std::endl;*/

	
	
	/*size_t alignment = sizeof(glm::mat4);
	if (shaderBufferType == ShaderBufferType::SSBO) {
		size_t minSboAlignment = props.limits.minStorageBufferOffsetAlignment;
		shaderBufferAlignment = (alignment + minSboAlignment - 1) & ~(minSboAlignment - 1);
	}
	else if (shaderBufferType == ShaderBufferType::UBO) {
		size_t minUboAlignment = props.limits.minUniformBufferOffsetAlignment;
		shaderBufferAlignment = (alignment + minUboAlignment - 1) & ~(minUboAlignment - 1);
	}
	else {
		throw std::runtime_error("Uknown shaderBufferType used");
	}*/

	size_t sboSize = props.limits.maxStorageBufferRange;
	//std::cout << "Max amount of objects in storage buffer " << sboSize / shaderBufferAlignment << std::endl;
}

void GraphicsEngine::createProjViewUBO() {
	VkDeviceSize bufferSize = sizeof(UniformBufferObject);

	projViewBuffers.resize(swapChainImages.size());
	projViewBufferMemory.resize(swapChainImages.size());

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		createBuffer(
			bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			projViewBuffers[i], projViewBufferMemory[i]
		);

		void* data;
		vkMapMemory(device, projViewBufferMemory[i], 0, sizeof(UniformBufferObject), 0, &data);
		memcpy(data, &UBO, sizeof(UniformBufferObject));
		vkUnmapMemory(device, projViewBufferMemory[i]);
	}
}

void GraphicsEngine::createModelUBOs() {
	VkDeviceSize bufferSize = sizeof(glm::mat4);

	modelBuffers.resize(swapChainImages.size() * rectCount);
	modelBufferMemory.resize(swapChainImages.size() * rectCount);
	mappedDeviceMemPtrs.resize(swapChainImages.size() * rectCount);

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		for (size_t j = 0; j < rectCount; j++) {
			createBuffer(
				bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				modelBuffers[i * rectCount + j], modelBufferMemory[i * rectCount + j]
			);

			vkMapMemory(device, modelBufferMemory[i * rectCount + j], 0, bufferSize, 0, &mappedDeviceMemPtrs[i * rectCount + j]);
		}
	}
}

void GraphicsEngine::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer, VkDeviceMemory &bufferMemory) {
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create buffer");
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(
		memRequirements.memoryTypeBits, properties
	);

	if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
		throw std::runtime_error("Failed to allocate buffer memory");
	}

	vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

void GraphicsEngine::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands();

	VkBufferCopy copyRegion{};
	copyRegion.size = size;

	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(commandBuffer);
}

VkCommandBuffer GraphicsEngine::beginSingleTimeCommands() {
	// VK_COMMAND_POOL_CREATE_TRANSIENT_BIT if creating a separate command pool for transfering the buffer
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = commandPool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

void GraphicsEngine::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(graphicsQueue);

	vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void GraphicsEngine::createDescriptorPool() {
	std::vector<VkDescriptorPoolSize> poolSizes(4);

	// Scene
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = static_cast<uint32_t>(swapChainImages.size());

	poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	poolSizes[1].descriptorCount = static_cast<uint32_t>(swapChainImages.size());

	// Material
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[2].descriptorCount = static_cast<uint32_t>(swapChainImages.size() * materials.size());

	// Object
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[3].descriptorCount = static_cast<uint32_t>(swapChainImages.size() * rectCount);

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = static_cast<uint32_t>(swapChainImages.size() * (2 + materials.size() + rectCount));

	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create descriptor pool");
	}
}

void GraphicsEngine::createDescriptorSets() {
	descriptorSets.resize(1 + swapChainImages.size() * 2);

	// Scene
	std::vector<VkDescriptorSetLayout> sceneLayouts(swapChainImages.size(), descriptorSetLayout[0]);

	VkDescriptorSetAllocateInfo sceneAllocInfo{};
	sceneAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	sceneAllocInfo.descriptorPool = descriptorPool;
	sceneAllocInfo.descriptorSetCount = static_cast<uint32_t>(swapChainImages.size());
	sceneAllocInfo.pSetLayouts = sceneLayouts.data();

	descriptorSets[0].resize(swapChainImages.size());
	if (vkAllocateDescriptorSets(device, &sceneAllocInfo, descriptorSets[0].data()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to allocate descriptor sets");
	}


	// Material
	std::vector<VkDescriptorSetLayout> materialLayouts(materials.size(), descriptorSetLayout[1]);

	VkDescriptorSetAllocateInfo materialAllocInfo{};
	materialAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	materialAllocInfo.descriptorPool = descriptorPool;
	materialAllocInfo.descriptorSetCount = static_cast<uint32_t>(materials.size());
	materialAllocInfo.pSetLayouts = materialLayouts.data();

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		descriptorSets[1 + i].resize(materials.size());

		if (vkAllocateDescriptorSets(device, &materialAllocInfo, descriptorSets[1 + i].data()) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate descriptor sets");
		}
	}


	// Object
	std::vector<VkDescriptorSetLayout> objectLayouts(rectCount, descriptorSetLayout[2]);

	VkDescriptorSetAllocateInfo objectAllocInfo{};
	objectAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	objectAllocInfo.descriptorPool = descriptorPool;
	objectAllocInfo.descriptorSetCount = static_cast<uint32_t>(rectCount);
	objectAllocInfo.pSetLayouts = objectLayouts.data();

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		descriptorSets[1 + swapChainImages.size() + i].resize(rectCount);

		if (vkAllocateDescriptorSets(device, &objectAllocInfo, descriptorSets[1 + swapChainImages.size() + i].data()) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate descriptor sets");
		}
	}
}

void GraphicsEngine::updateDescriptorSets() {
	// Scene
	for (size_t i = 0; i < swapChainImages.size(); i++) {
		std::vector<VkWriteDescriptorSet> writes;

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = projViewBuffers[i];
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		// Uniform buffer for the projection & view matrix
		VkWriteDescriptorSet bufferDescriptorWrite{};
		bufferDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		bufferDescriptorWrite.dstSet = descriptorSets[0][i];
		bufferDescriptorWrite.dstBinding = 0;
		bufferDescriptorWrite.dstArrayElement = 0;
		bufferDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bufferDescriptorWrite.descriptorCount = 1;
		bufferDescriptorWrite.pBufferInfo = &bufferInfo;

		writes.push_back(bufferDescriptorWrite);

		// Sampler
		VkDescriptorImageInfo samplerInfo{};
		samplerInfo.sampler = textureSampler;

		VkWriteDescriptorSet samplerDescriptorWrite{};
		samplerDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		samplerDescriptorWrite.dstSet = descriptorSets[0][i];
		samplerDescriptorWrite.dstBinding = 1;
		samplerDescriptorWrite.dstArrayElement = 0;
		samplerDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerDescriptorWrite.descriptorCount = 1;
		samplerDescriptorWrite.pImageInfo = &samplerInfo;

		writes.push_back(samplerDescriptorWrite);

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}


	// Material
	for (size_t i = 0; i < swapChainImages.size(); i++) {
		for (size_t j = 0; j < materials.size(); j++) {
			std::vector<VkWriteDescriptorSet> writes;

			// Sampled image
			VkDescriptorImageInfo imageInfo{};
			imageInfo.sampler = nullptr;
			imageInfo.imageView = textureImageViews[j];
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkWriteDescriptorSet imagesDescriptorWrite{};
			imagesDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			imagesDescriptorWrite.dstSet = descriptorSets[1 + i][j];
			imagesDescriptorWrite.dstBinding = 0;
			imagesDescriptorWrite.dstArrayElement = 0;
			imagesDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			imagesDescriptorWrite.descriptorCount = 1;
			imagesDescriptorWrite.pImageInfo = &imageInfo;

			writes.push_back(imagesDescriptorWrite);			

			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}

	// Object
	for (size_t i = 0; i < swapChainImages.size(); i++) {
		for (size_t j = 0; j < rectCount; j++) {
			std::vector<VkWriteDescriptorSet> writes;

			// Uniform buffer for model matrix
			VkDescriptorBufferInfo bufferInfo{};
			bufferInfo.buffer = modelBuffers[i * rectCount + j];
			bufferInfo.offset = 0;
			bufferInfo.range = sizeof(glm::mat4);

			VkWriteDescriptorSet bufferDescriptorWrite{};
			bufferDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			bufferDescriptorWrite.dstSet = descriptorSets[1 + swapChainImages.size() + i][j];
			bufferDescriptorWrite.dstBinding = 0;
			bufferDescriptorWrite.dstArrayElement = 0;
			bufferDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			bufferDescriptorWrite.descriptorCount = 1;
			bufferDescriptorWrite.pBufferInfo = &bufferInfo;

			writes.push_back(bufferDescriptorWrite);

			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}	
}

void GraphicsEngine::createCommandBuffer() {
	commandBuffers.resize(swapChainFramebuffers.size());

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size()); // Static_cast instead?

	if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
		throw std::runtime_error("Failed to allocate command buffers");
	}
}

void GraphicsEngine::recordCommandBuffer() {
	for (size_t i = 0; i < swapChainImages.size(); i++) {
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = 0; // USAGE_ONE_TIME_SUBMIT_BIT, USAGE_RENDER_PASS_CONTINUE_BIT, USAGE_SIMULTANEOUS_USE_BIT
		beginInfo.pInheritanceInfo = nullptr; // If secondary buffer inherited from primary buffer

		if (vkBeginCommandBuffer(commandBuffers[i], &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("Failed to begin recording command buffer");
		}

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = renderPass;
		renderPassInfo.framebuffer = swapChainFramebuffers[i];

		renderPassInfo.renderArea.offset = { 0,0 };
		renderPassInfo.renderArea.extent = swapChainExtent;

		VkClearValue clearColor = { 0.55f, 0.9f, 1.0f, 1.0f };
		renderPassInfo.clearValueCount = 1;
		renderPassInfo.pClearValues = &clearColor;

		vkCmdBeginRenderPass(commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE); // CONTENTS_SECONDARY_COMMAND_BUFFERS

		vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

		// Bind the projection & view matrix
		vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[0][1], 0, nullptr);

		if (rectCount > 0) {
			int rectIndex = 0;

			// For each material
			for (size_t j = 0; j < materials.size(); j++) {
				if (materials[j].count == 0) break;

				VkBuffer vertexBuffers[] = { materials[j].vertexBuffer };
				VkDeviceSize offsets[] = { 0 };
				vkCmdBindVertexBuffers(commandBuffers[i], 0, 1, vertexBuffers, offsets);

				vkCmdBindIndexBuffer(commandBuffers[i], materials[j].indexBuffer, 0, VK_INDEX_TYPE_UINT32);

				// Bind material descriptor set
				vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &descriptorSets[1 + i][j], 0, nullptr);

				// For each object
				for (size_t h = 0; h < materials[j].count; h++) {
					// Bind the model matrix
					vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 2, 1, &descriptorSets[1 + swapChainImages.size() + i][rectIndex++], 0, nullptr);

					// Draw one rect
					vkCmdDrawIndexed(commandBuffers[i], 6, 1, static_cast<uint32_t>(h * 6), 0, 0);
				}
			}
		}

		vkCmdEndRenderPass(commandBuffers[i]);

		if (vkEndCommandBuffer(commandBuffers[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to end recording command buffer");
		}
	}
}

void GraphicsEngine::createSyncObjects() {
	imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
	imagesInFlight.resize(swapChainImages.size(), VK_NULL_HANDLE);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;


	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS
			||
			vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS
			||
			vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create semaphores");
		}
	}
}

void GraphicsEngine::recreateSwapChain() {
	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);

	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(window, &width, &height);
		glfwWaitEvents();
	}

	vkDeviceWaitIdle(device);

	cleanupSwapChain();

	createSwapChain();
	createImageViews();
	createRenderPass();
	createGraphicsPipeline(); // Could use dynamic state instead
	createFramebuffers();

	// If swapChainImage count changes
	// Add aligned_free, free/destroy uniform/storage, destroy descriptorPool and destroy descriptorSets to cleanSwapChain
	//createUniformBuffer();
	//createStorageBuffer();
	//createDescriptorPool();
	//createDescriptorSets();

	vkResetCommandPool(device, commandPool, 0);
}

uint32_t GraphicsEngine::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	throw std::runtime_error("Failed to find suitable memory type");
}

void GraphicsEngine::setMouseCallback(GLFWmousebuttonfun fun) {
	glfwSetMouseButtonCallback(window, fun);
}

void GraphicsEngine::setupDescriptors() {
	createModelUBOs();
	createDescriptorPool();
	createDescriptorSets();
	updateDescriptorSets();
}