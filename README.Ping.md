# Ping — OFM-Network

Comprehensive ping implementation with internal queue, parallel slot management, and optional retry logic.

---

## Enabling Ping

Add the following to `platformio.ini` or `platformio.custom.ini`:

```ini
build_flags =
    -DOPENKNX_PING
```

Without `OPENKNX_PING`, all ping code is excluded from compilation (saves flash memory).

---

## Compile Flags

```
OPENKNX_PING_TIMEOUT     – Default timeout in milliseconds (default: 1000)
OPENKNX_PING_PARALLEL    – Max. concurrent pings (default: 5)
```

Additional pings are automatically queued when all slots are in use.

---

## API

### Standard Ping (with RTT measurement)

```cpp
// Ping by IP address
openknxNetwork.ping(IPAddress(192, 168, 1, 1), 
    [](IPAddress ip, bool reachable, uint32_t rttMs) {
        if (reachable) 
            openknx.logger.logWithValues("Ping", "%s: %lu ms", ip.toString().c_str(), rttMs);
    });

// Ping by hostname (DNS automatically resolved)
openknxNetwork.ping("router.local", 
    [](IPAddress ip, bool reachable, uint32_t rttMs) {
        if (reachable) 
            openknx.logger.logWithValues("Ping", "%s: %lu ms", ip.toString().c_str(), rttMs);
    });

// With explicit timeout (ms)
openknxNetwork.ping(target, callback, 500);
```

**Signature:**
```cpp
void ping(IPAddress target, 
          std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);

void ping(const std::string &host, 
          std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
```

---

### Retry Ping (simple bool callback)

Ping with automatic retries until the first successful reply.

```cpp
// Ping with automatic retries
openknxNetwork.ping(IPAddress(192, 168, 13, 254), 
    [](IPAddress ip, bool reachable) {
        if (reachable) 
            openknx.logger.logWithPrefix("Ping", "Host is reachable");
        else 
            openknx.logger.logWithPrefix("Ping", "Host not reachable (all retries exhausted)");
    },
    2,      // retries: 1 initial + 2 retries = 3 total attempts
    1000);  // timeoutMs (default: OPENKNX_PING_TIMEOUT)

// With hostname and retries
openknxNetwork.ping("gateway.local", 
    [](IPAddress ip, bool reachable) {
        /* ... */
    },
    3,      // retries: 1 initial + 3 retries = 4 total attempts
    500);   // 500ms timeout per attempt
```

**Signature:**
```cpp
void ping(IPAddress target, 
          std::function<void(IPAddress, bool)> callback,
          uint8_t retries = 2, 
          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);

void ping(const std::string &host, 
          std::function<void(IPAddress, bool)> callback,
          uint8_t retries = 2, 
          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
```

**Behavior:**
- As soon as **one ping replies** → Callback with `true`, **no further attempts**
- If a ping times out → Retry as long as `retries > 0`
- All retries exhausted without reply → Callback with `false`

**Note:** `retries` parameter specifies the number of **retry attempts** (not total attempts).  
Example: `retries=2` means 1 initial attempt + 2 retries = **3 total attempts**.

---

## Console Command

```
ping <ip|hostname>
```

Examples:
```
ping 8.8.8.8
ping openknx.de
ping 192.168.1.254
```

---

## Internal Mechanics

### Architecture

- **RetryContext** (shared_ptr): Manages the state of a retry-ping series
- **weak_ptr**: Prevents circular references and memory leaks
- **Queue**: Additional pings are automatically queued when capacity is full
- **DNS-Enqueue** (ESP32): Prevents deadlocks by enqueueing DNS results

### Thread Safety

- **RP2040**: Single-threaded lwIP — all operations from `loop()` are safe
- **ESP32**: Multi-threaded — DNS callbacks are enqueued, safe processing in `loop()`

### Memory Management

The retry implementation uses `shared_ptr`/`weak_ptr` to:
- Avoid circular references
- Exclude memory leaks
- Automatic cleanup after final callback

---

## Practical Examples

### Health Check Every 10 Seconds

```cpp
uint32_t _lastPingTime = 0;

void loop() {
    // ... other code ...
    
    if (delayCheck(_lastPingTime, 10000)) {
        openknxNetwork.ping(IPAddress(192, 168, 13, 254), 
            [](IPAddress ip, bool reachable) {
                openknx.logger.logWithPrefix("Ping", 
                    reachable ? "Gateway online" : "Gateway offline");
            }, 2);  // 2 retries
        _lastPingTime = millis();
    }
}
```

### Multiple Pings with Different Parameters

```cpp
// Standard ping with RTT measurement
openknxNetwork.ping("server1.local", [](IPAddress ip, bool ok, uint32_t rtt) {
    if (ok) logInfo("Server1", "RTT: %lu ms", rtt);
}, 500);

// Retry ping for critical host
openknxNetwork.ping(IPAddress(10, 0, 0, 1), [](IPAddress ip, bool ok) {
    if (ok) logInfo("Router", "Reachable");
}, 5, 1000);  // 5 retries, 1000ms timeout

// Simple ping without handler
openknxNetwork.ping("example.com");
```

### Check Before Network Operation

```cpp
void sendData() {
    openknxNetwork.ping("api.example.com", [](IPAddress ip, bool reachable) {
        if (reachable) {
            // Network available — send data
            performNetworkOperation();
        } else {
            // Network unavailable — retry later
            scheduleRetry();
        }
    }, 1);  // 1 retry (fast)
}
```

---

## Performance Tips

1. **Retry ping for non-critical checks**: Saves RTT measurement, faster on failure
2. **Batch pings**: Multiple pings are processed in parallel (up to `OPENKNX_PING_PARALLEL`)
3. **Adjust timeouts**: 
   - Local network: 500ms
   - Internet: 1000–2000ms
   - Slow networks: 3000ms+
4. **Hostname caching**: DNS results are cached by the system; frequent pings to the same URL are fast
