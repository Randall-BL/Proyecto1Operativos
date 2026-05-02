// Sketch de prueba para sensor ultrasonico HC-SR04 (ESP32)
// Permite cambiar pines y periodo por Serial:
//   "pins <trig> <echo>"  -> setea pines TRIG y ECHO
//   "poll <ms>"           -> setea periodo de sondeo
//   "start" / "stop"    -> inicia/detiene mediciones

const int DEFAULT_TRIG = 23; // TRIG por defecto
const int DEFAULT_ECHO = 22; // ECHO por defecto
int trigPin = DEFAULT_TRIG;
int echoPin = DEFAULT_ECHO;
unsigned long pollMs = 200UL;
bool running = true;
unsigned long lastMillis = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  digitalWrite(trigPin, LOW);
  Serial.println("Proximity test iniciado. Envia 'help' por Serial para comandos.");
}

float measure_cm() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  unsigned long dur = pulseIn(echoPin, HIGH, 30000UL); // 30ms timeout
  if (dur == 0) return 999.0f; // sin eco
  float dist = (dur * 0.0343f) / 2.0f; // velocidad sonido ~343 m/s
  return dist;
}

void applyPins() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  digitalWrite(trigPin, LOW);
  Serial.print("Pines aplicados TRIG="); Serial.print(trigPin);
  Serial.print(" ECHO="); Serial.println(echoPin);
}

void handleSerial() {
  if (Serial.available() == 0) return;
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() == 0) return;
  if (s.equalsIgnoreCase("help")) {
    Serial.println("Comandos:\n  pins <trig> <echo>\n  poll <ms>\n  start\n  stop\n  status");
    return;
  }
  if (s.startsWith("pins ")) {
    int a = s.indexOf(' ');
    int b = s.indexOf(' ', a+1);
    if (b <= a) { Serial.println("Uso: pins <trig> <echo>"); return; }
    String t = s.substring(a+1, b);
    String e = s.substring(b+1);
    int nt = t.toInt();
    int ne = e.toInt();
    if (nt < 0 || ne < 0) { Serial.println("Pines invalidos"); return; }
    trigPin = nt; echoPin = ne;
    applyPins();
    return;
  }
  if (s.startsWith("poll ")) {
    String v = s.substring(5);
    unsigned long ms = (unsigned long)v.toInt();
    if (ms < 20) ms = 20;
    pollMs = ms;
    Serial.print("pollMs="); Serial.println(pollMs);
    return;
  }
  if (s.equalsIgnoreCase("start")) { running = true; Serial.println("Medicion START"); return; }
  if (s.equalsIgnoreCase("stop")) { running = false; Serial.println("Medicion STOP"); return; }
  if (s.equalsIgnoreCase("status")) {
    Serial.print("TRIG="); Serial.print(trigPin);
    Serial.print(" ECHO="); Serial.println(echoPin);
    Serial.print("pollMs="); Serial.println(pollMs);
    Serial.print("running="); Serial.println(running?"YES":"NO");
    return;
  }
  Serial.println("Comando no reconocido. Escribe 'help'.");
}

void loop() {
  handleSerial();
  unsigned long now = millis();
  if (!running) return;
  if (now - lastMillis < pollMs) return;
  lastMillis = now;
  float d = measure_cm();
  if (d >= 999.0f) Serial.println("Distance: out of range");
  else {
    Serial.print("Distance: "); Serial.print(d, 1); Serial.println(" cm");
  }
}
