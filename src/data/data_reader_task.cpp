#include "data_reader_task.h"
#include "decoding/dlms_decoder.h"
#include "decoding/ascii_decoder.h"
#include "p1data_funcs.h"
#include "debug.h"
#include "../zap_log.h" // Added for logging

// Define TAG for logging
static constexpr LogTag TAG = LogTag("data_reader_task", ZLOG_LEVEL_INFO);

DataReaderTask::DataReaderTask(uint32_t stackSize, UBaseType_t priority) 
    : taskHandle(nullptr), stackSize(stackSize), priority(priority), shouldRun(false),
      p1DataQueue(nullptr), readInterval(10000), lastReadTime(0), baudRateIx(0) {
}

DataReaderTask::~DataReaderTask() {
    stop();
}

void DataReaderTask::begin(QueueHandle_t dataQueue) {
    if (taskHandle != nullptr) {
        return; // Task already running
    }

    // Set up the frame callback before initializing
    p1Meter.setFrameCallback([this](const IFrameData& frame) {
        this->handleFrame(frame);
    });

    if (!p1Meter.begin(p1Meter.getConfig(baudRateIx))) {
        LOG_TE(TAG, "Failed to initialize P1 meter");
    }

    LOG_TI(TAG, "P1 meter initialized with baud rate %d", p1Meter.getConfig(baudRateIx).baudRate);
    this->p1DataQueue = dataQueue;
    shouldRun = true;
    xTaskCreatePinnedToCore(
        taskFunction,
        "DataReaderTask",
        stackSize,
        this,
        priority,
        &taskHandle,
        0  // Run on core 0
    );

    LOG_TI(TAG, "DataReaderTask started with stack size %d and priority %d", stackSize, priority);
}

void DataReaderTask::stop() {
    if (taskHandle == nullptr) {
        return; // Task not running
    }
    
    shouldRun = false;
    vTaskDelay(pdMS_TO_TICKS(100)); // Give the task time to exit
    
    if (taskHandle != nullptr) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
    }
}

void DataReaderTask::setInterval(uint32_t interval) {
    readInterval = interval;
}

void DataReaderTask::enqueueData(const P1Data& p1data) {
    if (p1DataQueue != nullptr) {
        // Create a data package with char array, this is ok as the xQueueSendToBack function will copy the data
        DataPackage package;
        
        // Clear the data buffer first
        
        // Check if the JWT was created successfully
        if (createP1JWTPayload(p1data, package.data, MAX_DATA_SIZE)) {
            LOG_TE(TAG, "Failed to create JWT");
            return;
        }
        // Copy the JWT to the data object
        package.timestamp = millis();
        if (uxQueueSpacesAvailable(p1DataQueue) == 0) {
            // Queue is full, remove the oldest item first
            DataPackage oldPackage;
            xQueueReceive(p1DataQueue, &oldPackage, 0);
            LOG_TW(TAG, "Queue full, removed oldest item");
        }
        
        // Add the new package to the back (FIFO behavior)
        BaseType_t result = xQueueSendToBack(p1DataQueue, &package, pdMS_TO_TICKS(100));
        if (result == pdPASS) {
            LOG_TD(TAG, "Added data package to queue");
        } else {
            LOG_TE(TAG, "Failed to add data package to queue");
        }            
    }
}

// New method to handle complete frames received from P1Meter
void DataReaderTask::handleFrame(const IFrameData& frame) {
    
    const size_t frameSize = frame.getFrameSize();

    // Debug output print first 15 and last 15 bytes of the frame
    // LOG_TD(TAG, "Received P1 frame (%d bytes)", size);
    // for (size_t i = 0; i < min(size, size_t(15)); i++) {
    //     LOG_TD(TAG, "%c", (char)frame.getFrameByte(i));
    // }
    // if (size > 30) LOG_TD(TAG, "...");
    // for (size_t i = max(size - 15, size_t(15)); i < size; i++) {
    //     LOG_TD(TAG, "%c", (char)frame.getFrameByte(i));
    // }
    
    // Decode the frame
    DLMSDecoder decoder;
    AsciiDecoder asciiDecoder;
    P1Data p1data;
    bool isDecoded = false;

    switch (frame.getFrameTypeId()) {
        case IFrameData::Type::FRAME_TYPE_HDLC:
            LOG_TD(TAG, "DLMS frame detected");
            if (decoder.decodeBuffer(frame, p1data)) {
                LOG_TI(TAG, "DLMS data decoded successfully");
                isDecoded = true;
            }
            break;
        case IFrameData::Type::FRAME_TYPE_ASCII:
            LOG_TD(TAG, "ASCII frame detected");
            if (asciiDecoder.decodeBuffer(frame, p1data)) {
                LOG_TI(TAG, "ASCII data decoded successfully");
                isDecoded = true;
            }
            break;
        case IFrameData::Type::FRAME_TYPE_MBUS:
            LOG_TD(TAG, "M-Bus frame detected");
            // M-Bus decoding is not implemented yet, but we can log it
            isDecoded=false;
            break;
        default:
            LOG_TW(TAG, "Unknown frame type");
            break;
    }

    LOG_TI(TAG, "Frame decoded %s", isDecoded ? "true" : "false");
    
    if (isDecoded) {
        Debug::addFrame();
        lastReadTime = millis();
        p1data.setTimeStamp();
        enqueueData(p1data);
        
        lastDecodedData = p1data; // Store the last decoded data for potential future use
    } else {
        Debug::addFailedFrame();
        Debug::clearFaultyFrameData();
        for (size_t i = 0; i < frameSize; i++) {
            Debug::addFaultyFrameData(frame.getFrameByte(i));
        }
        LOG_TE(TAG, "Failed to decode P1 data frame");
    }
}

void DataReaderTask::rotateP1MeterBaudRate() {
    if (p1Meter.getNumConfigs() > 1) {
        baudRateIx = (baudRateIx + 1) % p1Meter.getNumConfigs();
        
        Debug::setP1MeterConfigIndex(baudRateIx);
        LOG_TD(TAG, "Rotating P1 meter condfig to %d", baudRateIx);
        
        if (!p1Meter.begin(p1Meter.getConfig(baudRateIx))) {
            LOG_TE(TAG, "Failed to reinitialize P1 meter with config ix: %d", baudRateIx);
        }    }

    lastReadTime = millis();
}

void DataReaderTask::taskFunction(void* parameter) {
    DataReaderTask* task = static_cast<DataReaderTask*>(parameter);

    
    while (task->shouldRun) {
        // Update P1 meter - this will read available data and call our frame callback
        // when complete frames are detected
        task->p1Meter.update();

        if (millis() - task->lastReadTime > 30 * 1000) { // 30 seconds without reading data
            task->rotateP1MeterBaudRate();
        }
        
        // Small delay to prevent task from hogging CPU
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Task cleanup
    vTaskDelete(NULL);
}