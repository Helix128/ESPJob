# ESPJob 
## Job system library for ESP32

Tiny (header-only) a-la C# job system for ESP32 (wraps FreeRTOS vTask functions). 
Run tasks with optional callbacks, delays, core selection and priority control.
Also comes with a few handy macros to simplify lambda and callback definitions.

# Quick usage example

```cpp
#include <Arduino.h>
#include <ESPJob.h>

void setup(){
  Serial.begin(115200);

  while(!Serial){
    delay(10);
  }

  JobHandle job = Job::RunOnCore(0, LAMBDA(int) {
      Serial.printf("Job started on core %d\n", Job::GetRunningCore());
      delay(2000); // Simulate a long task
      Serial.println("Job completed");
      return 67;
  }, CALLBACK(int result) {
      Serial.printf("Job callback with result: %d\n", result);
  });
  
  Serial.println("Main setup completed");
}

void loop(){
    Job::update();
}
```
## Output:
```
Main setup completed
Job started on core 0
Job completed
Job callback with result: 67
```
