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