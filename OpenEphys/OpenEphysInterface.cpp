//
//  OpenEphysInterface.cpp
//  OpenEphys
//
//  Created by Christopher Stawarz on 7/22/15.
//  Copyright (c) 2015 The MWorks Project. All rights reserved.
//

#include "OpenEphysInterface.h"


BEGIN_NAMESPACE_MW


BEGIN_NAMESPACE()


inline void logZMQError(const std::string &message) {
    merror(M_IODEVICE_MESSAGE_DOMAIN, "%s: %s", message.c_str(), zmq_strerror(zmq_errno()));
}


inline MWTime currentTimeUS() {
    return Clock::instance()->getCurrentTimeUS();
}


inline MWTime secsToUS(double timestamp) {
    return MWTime(timestamp * 1e6);
}


struct OpenEphysEvent {
    
    struct TTL {
        std::uint8_t nodeID;
        std::uint8_t eventID;  // Line state
        std::uint8_t eventChannel;
        /* Other fields ignored */
    };
    
    struct Spike {
        std::int64_t timestamp;
        std::int64_t timestampSoftware;
        std::uint16_t _source;  // Used internally by spike detector
        std::uint16_t nChannels;
        std::uint16_t nSamples;
        std::uint16_t sortedID;
        std::uint16_t electrodeID;
        std::uint16_t channel;
        /* Other fields ignored */
    } __attribute__((packed));
    
    union {
        TTL ttl;
        Spike spike;
    };
    
};

// Verify packing
BOOST_STATIC_ASSERT(sizeof(OpenEphysEvent::TTL) == 3);
BOOST_STATIC_ASSERT(sizeof(OpenEphysEvent::Spike) == 28);
BOOST_STATIC_ASSERT(sizeof(OpenEphysEvent) == sizeof(OpenEphysEvent::Spike));


END_NAMESPACE()


const std::string OpenEphysInterface::HOSTNAME("hostname");
const std::string OpenEphysInterface::PORT("port");
const std::string OpenEphysInterface::SYNC("sync");
const std::string OpenEphysInterface::SYNC_CHANNELS("sync_channels");
const std::string OpenEphysInterface::SPIKES("spikes");


void OpenEphysInterface::describeComponent(ComponentInfo &info) {
    IODevice::describeComponent(info);
    
    info.setSignature("iodevice/open_ephys_interface");
    
    info.addParameter(HOSTNAME);
    info.addParameter(PORT);
    info.addParameter(SYNC);
    info.addParameter(SYNC_CHANNELS);
    info.addParameter(SPIKES, false);
}


OpenEphysInterface::OpenEphysInterface(const ParameterValueMap &parameters) :
    IODevice(parameters),
    endpoint("tcp://" + parameters[HOSTNAME].str() + ":" + parameters[PORT].str()),
    sync(parameters[SYNC]),
    zmqContext(nullptr, zmq_ctx_term),
    zmqSocket(nullptr, zmq_close),
    running(false)
{
    std::vector<Datum> syncChannelsValues;
    ParsedExpressionVariable::evaluateExpressionList(parameters[SYNC_CHANNELS].str(), syncChannelsValues);
    for (auto &channel : syncChannelsValues) {
        auto channelNumber = channel.getInteger();
        if (channelNumber < 1 || channelNumber > 8) {
            throw SimpleException(M_IODEVICE_MESSAGE_DOMAIN, "Invalid sync channel number");
        }
        if (!syncChannels.empty() && (channelNumber <= syncChannels.back())) {
            throw SimpleException(M_IODEVICE_MESSAGE_DOMAIN, "Sync channel numbers must be in ascending order");
        }
        syncChannels.push_back(channelNumber - 1);
    }
    if (syncChannels.empty()) {
        throw SimpleException(M_IODEVICE_MESSAGE_DOMAIN, "At least one sync channel is required");
    }
    
    if (!parameters[SPIKES].empty()) {
        spikes = VariablePtr(parameters[SPIKES]);
    }
}


OpenEphysInterface::~OpenEphysInterface() {
    terminateEventHandlerThread();
}


bool OpenEphysInterface::initialize() {
    zmqContext.reset(zmq_ctx_new());
    if (!zmqContext) {
        logZMQError("Unable to create ZeroMQ context");
        return false;
    }
    
    zmqSocket.reset(zmq_socket(zmqContext.get(), ZMQ_SUB));
    if (!zmqSocket) {
        logZMQError("Unable to create ZeroMQ socket");
        return false;
    }
    
    // Set receive timeout to half of sync interval
    const int recvTimeout = (syncInterval / 2) / 1000;  // ms
    if (0 != zmq_setsockopt(zmqSocket.get(), ZMQ_RCVTIMEO, &recvTimeout, sizeof(recvTimeout))) {
        logZMQError("Unable to set ZeroMQ socket receive timeout");
        return false;
    }
    
    if (!subscribeToEventType(TTL) ||
        (spikes && !subscribeToEventType(SPIKE)))
    {
        return false;
    }
    
    return true;
}


bool OpenEphysInterface::startDeviceIO() {
    scoped_lock lock(mutex);
    
    if (!running) {
        if (0 != zmq_connect(zmqSocket.get(), endpoint.c_str())) {
            logZMQError("Unable to connect to Open Ephys GUI");
            return false;
        }
        
        eventHandlerThread = boost::thread([this]() {
            handleEvents();
        });
        
        running = true;
    }
    
    return true;
}


bool OpenEphysInterface::stopDeviceIO() {
    scoped_lock lock(mutex);
    
    if (running) {
        terminateEventHandlerThread();
        
        if (0 != zmq_disconnect(zmqSocket.get(), endpoint.c_str())) {
            logZMQError("Unable to disconnect from Open Ephys GUI");
            return false;
        }
        
        running = false;
    }
    
    return true;
}


bool OpenEphysInterface::subscribeToEventType(std::uint8_t type) {
    if (0 != zmq_setsockopt(zmqSocket.get(), ZMQ_SUBSCRIBE, &type, sizeof(type))) {
        logZMQError("Unable to establish ZeroMQ message filter");
        return false;
    }
    
    return true;
}


void OpenEphysInterface::handleEvents() {
    const std::size_t numSyncValues = 1 << syncChannels.size();
    
    MWTime lastSyncTime = 0;
    int lastSyncValue = 0;
    int currentOESyncValue = 0;
    MWTime oeClockOffset = 0;
    
    constexpr MWTime syncReceiptCheckInterval = 5000000;  // 5 seconds
    MWTime lastSyncReceivedTime = currentTimeUS();
    MWTime lastSyncReceiptCheckTime = lastSyncReceivedTime;
    
    while (true) {
        if (!lastSyncTime || (currentTimeUS() - lastSyncTime >= syncInterval)) {
            const int syncValue = (lastSyncValue + 1) % numSyncValues;
            
            int reversedSyncValue = 0;
            for (std::size_t i = 0; i < syncChannels.size(); i++) {
                reversedSyncValue <<= 1;
                reversedSyncValue |= (syncValue & (1 << i)) >> i;
            }
            sync->setValue(reversedSyncValue);
            
            lastSyncTime = currentTimeUS();  // Record time *after* sync value has been set
            lastSyncValue = syncValue;
        }
        
        {
            const MWTime currentSyncReceiptCheckTime = currentTimeUS();
            if (currentSyncReceiptCheckTime - lastSyncReceiptCheckTime >= syncReceiptCheckInterval) {
                merror(M_IODEVICE_MESSAGE_DOMAIN,
                       "No Open Ephys clock sync received after %g seconds",
                       std::round(double(currentSyncReceiptCheckTime - lastSyncReceivedTime) / 1e6));
                lastSyncReceiptCheckTime = currentSyncReceiptCheckTime;
            }
        }
        
        std::uint8_t eventType = 0;
        double eventTimestamp = 0.0;
        OpenEphysEvent event;
        std::memset(&event, 0, sizeof(event));
        
        if (-1 == zmq_recv(zmqSocket.get(), &eventType, sizeof(eventType), 0) ||
            -1 == zmq_recv(zmqSocket.get(), &eventTimestamp, sizeof(eventTimestamp), ZMQ_DONTWAIT) ||
            -1 == zmq_recv(zmqSocket.get(), &event, sizeof(event), ZMQ_DONTWAIT))
        {
            
            if (zmq_errno() != EAGAIN) {
                logZMQError("Received failed on ZeroMQ socket");
            }
            
        } else if (TTL == eventType) {
            
            for (std::size_t i = 0; i < syncChannels.size(); i++) {
                if (event.ttl.eventChannel == syncChannels.at(i)) {
                    const int mask = 1 << (syncChannels.size() - 1 - i);
                    if (event.ttl.eventID) {
                        currentOESyncValue |= mask;
                    } else {
                        currentOESyncValue &= ~mask;
                    }
                    
                    if (i == syncChannels.size() - 1) {
                        lastSyncReceivedTime = currentTimeUS();
                        lastSyncReceiptCheckTime = lastSyncReceivedTime;
                        
                        if (currentOESyncValue == lastSyncValue) {
                            oeClockOffset = lastSyncTime - secsToUS(eventTimestamp);
                        } else {
                            merror(M_IODEVICE_MESSAGE_DOMAIN,
                                   "Open Ephys clock sync values don't match: sent %d, received %d",
                                   lastSyncValue,
                                   currentOESyncValue);
                            
                            // Reset things and start over
                            lastSyncTime = 0;
                            lastSyncValue = 0;
                            currentOESyncValue = 0;
                        }
                    }
                    
                    break;
                }
            }
            
        } else if (SPIKE == eventType) {
            
            if (spikes) {
                Datum info(M_DICTIONARY, 4);
                info.addElement("oe_timestamp", event.spike.timestamp);
                info.addElement("sorted_id", event.spike.sortedID);
                info.addElement("electrode_id", event.spike.electrodeID);
                info.addElement("channel", event.spike.channel);
                spikes->setValue(info, secsToUS(eventTimestamp) + oeClockOffset);
            }
            
        } else {
            
            merror(M_IODEVICE_MESSAGE_DOMAIN, "Open Ephys event has unexpected type (%hhu)", eventType);
            
        }
    
        // Give another thread a chance to terminate this one
        boost::this_thread::interruption_point();
    }
}


void OpenEphysInterface::terminateEventHandlerThread() {
    if (eventHandlerThread.joinable()) {
        eventHandlerThread.interrupt();
        eventHandlerThread.join();
    }
}


END_NAMESPACE_MW




























