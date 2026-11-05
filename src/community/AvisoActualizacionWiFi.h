#ifndef AVISO_ACTUALIZACION_WIFI_H
#define AVISO_ACTUALIZACION_WIFI_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

namespace AvisoActualizacionWiFi {

static constexpr const char* URL_JSON =
    "https://backmeteor4090quintana.github.io/arcade-console-updates/update.json";

struct Aviso {
  bool consultaRealizada = false;
  bool disponible = false;
  bool versionNueva = false;
  String version;
  String titulo;
  String mensaje;
  String error;
};

static Aviso avisoActual;
static bool avisoPendiente = false;

inline String extraerStringJson(const String& json, const char* clave) {
  const String patron = String("\"") + clave + "\"";
  int pos = json.indexOf(patron);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + patron.length());
  if (pos < 0) return "";
  pos = json.indexOf('"', pos + 1);
  if (pos < 0) return "";

  String salida;
  bool escape = false;
  for (int i = pos + 1; i < (int)json.length(); ++i) {
    const char c = json[i];
    if (escape) {
      if (c == 'n') salida += '\n';
      else salida += c;
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      break;
    } else {
      salida += c;
    }
  }
  return salida;
}

inline bool extraerBoolJson(const String& json, const char* clave, bool defecto) {
  const String patron = String("\"") + clave + "\"";
  int pos = json.indexOf(patron);
  if (pos < 0) return defecto;
  pos = json.indexOf(':', pos + patron.length());
  if (pos < 0) return defecto;

  String valor = json.substring(pos + 1);
  valor.trim();
  if (valor.startsWith("true")) return true;
  if (valor.startsWith("false")) return false;
  return defecto;
}

inline void separarVersion(const String& version, int& mayor, int& menor, int& parche) {
  mayor = menor = parche = 0;
  String limpia = version;
  limpia.trim();
  if (limpia.startsWith("v") || limpia.startsWith("V")) limpia.remove(0, 1);

  const int p1 = limpia.indexOf('.');
  const int p2 = p1 >= 0 ? limpia.indexOf('.', p1 + 1) : -1;

  mayor = limpia.substring(0, p1 >= 0 ? p1 : limpia.length()).toInt();
  if (p1 >= 0) {
    menor = limpia.substring(p1 + 1, p2 >= 0 ? p2 : limpia.length()).toInt();
  }
  if (p2 >= 0) parche = limpia.substring(p2 + 1).toInt();
}

inline bool esVersionMayor(const String& nueva, const String& actual) {
  int nMa, nMe, nPa;
  int aMa, aMe, aPa;
  separarVersion(nueva, nMa, nMe, nPa);
  separarVersion(actual, aMa, aMe, aPa);

  if (nMa != aMa) return nMa > aMa;
  if (nMe != aMe) return nMe > aMe;
  return nPa > aPa;
}

inline bool yaFueAvisada(const String& version) {
  Preferences prefs;
  prefs.begin("arcadeUpdate", true);
  const String anterior = prefs.getString("avisada", "");
  prefs.end();
  return anterior == version;
}

inline void marcarComoAvisada(const String& version) {
  Preferences prefs;
  prefs.begin("arcadeUpdate", false);
  prefs.putString("avisada", version);
  prefs.end();
}

inline bool consultar(const char* versionActual) {
  avisoActual = Aviso();
  avisoPendiente = false;

  if (WiFi.status() != WL_CONNECTED) {
    avisoActual.error = "Wi-Fi sin conexion";
    return false;
  }

  WiFiClientSecure cliente;
  // Para el aviso no se descarga ni instala firmware. Se usa HTTPS y la
  // instalación permanece protegida por el flujo local de la tarjeta SD.
  cliente.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(cliente, URL_JSON)) {
    avisoActual.error = "No se pudo iniciar HTTPS";
    return false;
  }

  http.addHeader("Cache-Control", "no-cache");
  const int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    avisoActual.error = String("HTTP ") + codigo;
    http.end();
    return false;
  }

  const String json = http.getString();
  http.end();

  avisoActual.consultaRealizada = true;
  avisoActual.version = extraerStringJson(json, "version");
  avisoActual.titulo = extraerStringJson(json, "title");
  avisoActual.mensaje = extraerStringJson(json, "message");
  avisoActual.disponible = extraerBoolJson(json, "available", false);

  if (avisoActual.titulo.length() == 0) {
    avisoActual.titulo = "ACTUALIZACION DISPONIBLE";
  }
  if (avisoActual.mensaje.length() == 0) {
    avisoActual.mensaje = "Hay una nueva version para instalar desde la SD.";
  }

  avisoActual.versionNueva =
      avisoActual.version.length() > 0 &&
      esVersionMayor(avisoActual.version, String(versionActual));

  avisoPendiente =
      avisoActual.disponible &&
      avisoActual.versionNueva &&
      !yaFueAvisada(avisoActual.version);

  Serial.printf(
      "Aviso WiFi | actual %s | publicada %s | disponible %s | pendiente %s\n",
      versionActual,
      avisoActual.version.c_str(),
      avisoActual.disponible ? "SI" : "NO",
      avisoPendiente ? "SI" : "NO"
  );

  return true;
}

inline bool hayAvisoPendiente() {
  return avisoPendiente;
}

inline const Aviso& obtener() {
  return avisoActual;
}

inline void consumirAviso() {
  if (avisoActual.version.length() > 0) {
    marcarComoAvisada(avisoActual.version);
  }
  avisoPendiente = false;
}

}  // namespace AvisoActualizacionWiFi

#endif
