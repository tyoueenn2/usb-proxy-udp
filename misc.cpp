#include <math.h>
#include <sstream>

#include "misc.h"
#include "device-libusb.h"

std::string hexToAscii(std::string input) {
	std::string output = input;
	size_t pos = output.find("\\x");
	while (pos != std::string::npos) {
		std::string substr = output.substr(pos + 2, 2);

		std::istringstream iss(substr);
		iss.flags(std::ios::hex);
		int i;
		iss >> i;
		output = output.replace(pos, 4, 1, char(i));
		pos = output.find("\\x");
	}
	return output;
}

int hexToDecimal(int input) {
	int output = 0;
	int i = 0;
	while (input != 0) {
		output += (input % 10) * pow(16, i);
		input /= 10;
		i++;
	}
	return output;
}

// Parse raw hex string (e.g., "010203" or "01 02 03") into bytes
std::vector<uint8_t> parseHexString(const std::string& hex) {
	std::vector<uint8_t> bytes;
	std::string cleanHex;
	
	// Remove spaces and other non-hex characters
	for (char c : hex) {
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			cleanHex += c;
		}
	}
	
	// Parse pairs of hex digits
	for (size_t i = 0; i + 1 < cleanHex.length(); i += 2) {
		std::string byteStr = cleanHex.substr(i, 2);
		uint8_t byte = (uint8_t)std::stoul(byteStr, nullptr, 16);
		bytes.push_back(byte);
	}
	
	return bytes;
}

// Print hex dump of data for debugging
void printHexDump(const char* prefix, const uint8_t* data, size_t length) {
	printf("%s", prefix);
	for (size_t i = 0; i < length; i++) {
		printf("%02x ", data[i]);
		if ((i + 1) % 16 == 0 && i + 1 < length) {
			printf("\n%*s", (int)strlen(prefix), "");
		}
	}
	printf("\n");
}

// Save USB descriptors to a file
void saveUsbDescriptors(const std::string& filename) {
	Json::Value root;
	
	// Save device descriptor
	Json::Value device;
	device["bLength"] = device_device_desc.bLength;
	device["bDescriptorType"] = device_device_desc.bDescriptorType;
	device["bcdUSB"] = device_device_desc.bcdUSB;
	device["bDeviceClass"] = device_device_desc.bDeviceClass;
	device["bDeviceSubClass"] = device_device_desc.bDeviceSubClass;
	device["bDeviceProtocol"] = device_device_desc.bDeviceProtocol;
	device["bMaxPacketSize0"] = device_device_desc.bMaxPacketSize0;
	device["idVendor"] = device_device_desc.idVendor;
	device["idProduct"] = device_device_desc.idProduct;
	device["bcdDevice"] = device_device_desc.bcdDevice;
	device["iManufacturer"] = device_device_desc.iManufacturer;
	device["iProduct"] = device_device_desc.iProduct;
	device["iSerialNumber"] = device_device_desc.iSerialNumber;
	device["bNumConfigurations"] = device_device_desc.bNumConfigurations;
	root["device"] = device;
	
	// Save configuration descriptors
	Json::Value configs(Json::arrayValue);
	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		Json::Value config;
		config["bLength"] = device_config_desc[i]->bLength;
		config["bDescriptorType"] = device_config_desc[i]->bDescriptorType;
		config["wTotalLength"] = device_config_desc[i]->wTotalLength;
		config["bNumInterfaces"] = device_config_desc[i]->bNumInterfaces;
		config["bConfigurationValue"] = device_config_desc[i]->bConfigurationValue;
		config["iConfiguration"] = device_config_desc[i]->iConfiguration;
		config["bmAttributes"] = device_config_desc[i]->bmAttributes;
		config["MaxPower"] = device_config_desc[i]->MaxPower;
		
		// Save interfaces and endpoints
		Json::Value interfaces(Json::arrayValue);
		for (int j = 0; j < device_config_desc[i]->bNumInterfaces; j++) {
			Json::Value iface;
			int num_altsetting = device_config_desc[i]->interface[j].num_altsetting;
			iface["num_altsetting"] = num_altsetting;
			
			Json::Value altsettings(Json::arrayValue);
			for (int k = 0; k < num_altsetting; k++) {
				const struct libusb_interface_descriptor& alt = 
					device_config_desc[i]->interface[j].altsetting[k];
				
				Json::Value altJson;
				altJson["bLength"] = alt.bLength;
				altJson["bDescriptorType"] = alt.bDescriptorType;
				altJson["bInterfaceNumber"] = alt.bInterfaceNumber;
				altJson["bAlternateSetting"] = alt.bAlternateSetting;
				altJson["bNumEndpoints"] = alt.bNumEndpoints;
				altJson["bInterfaceClass"] = alt.bInterfaceClass;
				altJson["bInterfaceSubClass"] = alt.bInterfaceSubClass;
				altJson["bInterfaceProtocol"] = alt.bInterfaceProtocol;
				altJson["iInterface"] = alt.iInterface;
				
				// Save endpoints
				Json::Value endpoints(Json::arrayValue);
				for (int l = 0; l < alt.bNumEndpoints; l++) {
					Json::Value ep;
					ep["bLength"] = alt.endpoint[l].bLength;
					ep["bDescriptorType"] = alt.endpoint[l].bDescriptorType;
					ep["bEndpointAddress"] = alt.endpoint[l].bEndpointAddress;
					ep["bmAttributes"] = alt.endpoint[l].bmAttributes;
					ep["wMaxPacketSize"] = alt.endpoint[l].wMaxPacketSize;
					ep["bInterval"] = alt.endpoint[l].bInterval;
					ep["bRefresh"] = alt.endpoint[l].bRefresh;
					ep["bSynchAddress"] = alt.endpoint[l].bSynchAddress;
					endpoints.append(ep);
				}
				altJson["endpoints"] = endpoints;
				altsettings.append(altJson);
			}
			iface["altsettings"] = altsettings;
			interfaces.append(iface);
		}
		config["interfaces"] = interfaces;
		configs.append(config);
	}
	root["configurations"] = configs;
	
	// Write to file
	std::ofstream outFile(filename);
	if (outFile.is_open()) {
		Json::StyledWriter writer;
		outFile << writer.write(root);
		outFile.close();
		printf("USB descriptors saved to: %s\n", filename.c_str());
	} else {
		fprintf(stderr, "Failed to open file for writing: %s\n", filename.c_str());
	}
}
