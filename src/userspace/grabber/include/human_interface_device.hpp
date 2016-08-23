#pragma once

#include "boost_defs.hpp"

#include "apple_hid_usage_tables.hpp"
#include "logger.hpp"
#include "manipulator.hpp"
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDElement.h>
#include <IOKit/hid/IOHIDQueue.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDValue.h>
#include <boost/function.hpp>
#include <cstdint>
#include <list>
#include <unordered_map>

class human_interface_device final {
public:
  typedef boost::function<void(human_interface_device& device,
                               IOHIDValueRef _Nonnull value,
                               IOHIDElementRef _Nonnull element,
                               uint32_t usage_page,
                               uint32_t usage,
                               CFIndex integer_value)>
      value_callback;

  human_interface_device(IOHIDDeviceRef _Nonnull device) : device_(device),
                                                           queue_(nullptr),
                                                           grabbed_(false) {
    CFRetain(device_);

    auto elements = IOHIDDeviceCopyMatchingElements(device_, nullptr, kIOHIDOptionsTypeNone);
    if (elements) {
      for (CFIndex i = 0; i < CFArrayGetCount(elements); ++i) {
        // Skip unnecessary usages.
        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, i)));
        auto usage_page = IOHIDElementGetUsagePage(element);
        auto usage = IOHIDElementGetUsage(element);

        if (usage_page == kHIDPage_KeyboardOrKeypad) {
          if (usage < kHIDUsage_KeyboardA ||
              usage >= kHIDUsage_Keyboard_Reserved) {
            continue;
          }
        }

        // Add to elements_
        auto key = elements_key(usage_page, usage);
        if (elements_.find(key) == elements_.end()) {
          CFRetain(element);
          elements_[key] = element;
        }
      }
      CFRelease(elements);
    }
  }

  ~human_interface_device(void) {
    unschedule();

    if (grabbed_) {
      ungrab();
    } else {
      close();
    }

    if (queue_) {
      CFRelease(queue_);
    }

    for (const auto& it : elements_) {
      CFRelease(it.second);
    }
    elements_.clear();

    CFRelease(device_);
  }

  IOReturn open(IOOptionBits options = kIOHIDOptionsTypeNone) {
    return IOHIDDeviceOpen(device_, options);
  }

  IOReturn close(void) {
    return IOHIDDeviceClose(device_, kIOHIDOptionsTypeNone);
  }

  void schedule(void) {
    IOHIDDeviceScheduleWithRunLoop(device_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    if (queue_) {
      IOHIDQueueScheduleWithRunLoop(queue_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    }
  }

  void unschedule(void) {
    if (queue_) {
      IOHIDQueueUnscheduleFromRunLoop(queue_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    }
    IOHIDDeviceUnscheduleFromRunLoop(device_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
  }

  void grab(value_callback callback) {
    if (!device_) {
      return;
    }

    if (grabbed_) {
      ungrab();
    }

    IOReturn r = open(kIOHIDOptionsTypeSeizeDevice);
    if (r != kIOReturnSuccess) {
      logger::get_logger().error("IOHIDDeviceOpen error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, r);
      return;
    }

    const CFIndex depth = 1024;
    queue_ = IOHIDQueueCreate(kCFAllocatorDefault, device_, depth, kIOHIDOptionsTypeNone);
    if (queue_) {
      // Add all elements
      for (const auto& it : elements_) {
        IOHIDQueueAddElement(queue_, it.second);
      }

      value_callback_ = callback;
      IOHIDQueueRegisterValueAvailableCallback(queue_, static_queue_value_available_callback, this);

      IOHIDQueueStart(queue_);
    }

    schedule();

    grabbed_ = true;
  }

  void ungrab(void) {
    if (!device_) {
      return;
    }

    if (!grabbed_) {
      return;
    }

    unschedule();

    if (queue_) {
      IOHIDQueueRegisterValueAvailableCallback(queue_, nullptr, nullptr);
      IOHIDQueueStop(queue_);

      // Remove all elements
      for (const auto& it : elements_) {
        IOHIDQueueRemoveElement(queue_, it.second);
      }

      CFRelease(queue_);
      queue_ = nullptr;
    }

    IOReturn r = close();
    if (r != kIOReturnSuccess) {
      logger::get_logger().error("IOHIDDeviceClose error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, r);
      return;
    }

    grabbed_ = false;
  }

  long get_max_input_report_size(void) const {
    long value = 0;
    get_long_property(CFSTR(kIOHIDMaxInputReportSizeKey), value);
    return value;
  }

  long get_vendor_id(void) const {
    long value = 0;
    get_long_property(CFSTR(kIOHIDVendorIDKey), value);
    return value;
  }

  long get_product_id(void) const {
    long value = 0;
    get_long_property(CFSTR(kIOHIDProductIDKey), value);
    return value;
  }

  long get_location_id(void) const {
    long value = 0;
    get_long_property(CFSTR(kIOHIDLocationIDKey), value);
    return value;
  }

  std::string get_manufacturer(void) const {
    std::string value;
    get_string_property(CFSTR(kIOHIDManufacturerKey), value);
    return value;
  }

  std::string get_product(void) const {
    std::string value;
    get_string_property(CFSTR(kIOHIDProductKey), value);
    return value;
  }

  std::string get_serial_number_string(void) const {
    std::string value;
    get_string_property(CFSTR(kIOHIDSerialNumberKey), value);
    return value;
  }

  std::string get_transport(void) const {
    std::string value;
    get_string_property(CFSTR(kIOHIDTransportKey), value);
    return value;
  }

  bool get_long_property(const CFStringRef _Nonnull key, long& value) const {
    if (!device_) {
      return false;
    }

    auto property = IOHIDDeviceGetProperty(device_, key);
    if (!property) {
      return false;
    }

    if (CFNumberGetTypeID() != CFGetTypeID(property)) {
      return false;
    }

    return CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberLongType, &value);
  }

  bool get_string_property(const CFStringRef _Nonnull key, std::string& value) const {
    if (!device_) {
      return false;
    }

    auto property = IOHIDDeviceGetProperty(device_, key);
    if (!property) {
      return false;
    }

    if (CFStringGetTypeID() != CFGetTypeID(property)) {
      return false;
    }

    auto p = CFStringGetCStringPtr(static_cast<CFStringRef>(property), kCFStringEncodingUTF8);
    if (!p) {
      value.clear();
    } else {
      value = p;
    }
    return true;
  }

  std::unordered_map<manipulator::key_code, manipulator::key_code>& get_simple_changed_keys(void) {
    return simple_changed_keys_;
  }

  std::unordered_map<manipulator::key_code, manipulator::key_code>& get_fn_changed_keys(void) {
    return fn_changed_keys_;
  }

  size_t get_pressed_keys_count(void) const {
    return pressed_key_usages_.size();
  }

private:
  static void static_queue_value_available_callback(void* _Nullable context, IOReturn result, void* _Nullable sender) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<human_interface_device*>(context);
    if (!self) {
      return;
    }

    auto queue = static_cast<IOHIDQueueRef>(sender);
    if (!queue) {
      return;
    }

    self->queue_value_available_callback(queue);
  }

  void queue_value_available_callback(IOHIDQueueRef _Nonnull queue) {
    while (true) {
      auto value = IOHIDQueueCopyNextValueWithTimeout(queue, 0.);
      if (!value) {
        break;
      }

      auto element = IOHIDValueGetElement(value);
      if (element) {
        auto usage_page = IOHIDElementGetUsagePage(element);
        auto usage = IOHIDElementGetUsage(element);
        auto integer_value = IOHIDValueGetIntegerValue(value);

        // update pressed_key_usages_
        if ((usage_page == kHIDPage_KeyboardOrKeypad) ||
            (usage_page == kHIDPage_AppleVendorTopCase && usage == kHIDUsage_AV_TopCase_KeyboardFn)) {
          bool pressed = integer_value;
          uint64_t u = (static_cast<uint64_t>(usage_page) << 32) | usage;
          if (pressed) {
            pressed_key_usages_.push_back(u);
          } else {
            pressed_key_usages_.remove(u);
          }
        }

        // call value_callback_
        if (value_callback_) {
          value_callback_(*this, value, element, usage_page, usage, integer_value);
        }
      }

      CFRelease(value);
    }
  }

  uint64_t elements_key(uint32_t usage_page, uint32_t usage) {
    return (static_cast<uint64_t>(usage_page) << 32 | usage);
  }

  IOHIDDeviceRef _Nonnull device_;
  IOHIDQueueRef _Nullable queue_;
  std::unordered_map<uint64_t, IOHIDElementRef> elements_;

  bool grabbed_;
  std::list<uint64_t> pressed_key_usages_;
  value_callback value_callback_;

  std::unordered_map<manipulator::key_code, manipulator::key_code> simple_changed_keys_;
  std::unordered_map<manipulator::key_code, manipulator::key_code> fn_changed_keys_;
};
