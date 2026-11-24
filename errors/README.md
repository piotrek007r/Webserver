# 🚨 System Obsługi Błędów HTTP

System obsługi błędów HTTP

---

## 📋 Spis Treści

1. [Obsługiwane Kody Błędów](#obsługiwane-kody-błędów)
2. [Podstawowe Użycie](#podstawowe-użycie)
3. [Zaawansowane Przykłady](#zaawansowane-przykłady)
4. [Integracja z Serwerem](#integracja-z-serwerem)
5. [Wygląd Stron Błędów](#wygląd-stron-błędów)

---

## 🔢 Obsługiwane Kody Błędów

### ❌ 4xx - Błędy Klienta (Client Errors)

| Kod | Nazwa | Opis |
|-----|-------|------|
| **400** | Bad Request | Nieprawidłowa składnia żądania |
| **401** | Unauthorized | Wymagana autoryzacja |
| **402** | Payment Required | Wymagana płatność |
| **403** | Forbidden | Serwer odmawia dostępu |
| **404** | Not Found | Zasób nie został znaleziony |
| **405** | Method Not Allowed | Metoda HTTP nie jest obsługiwana |
| **406** | Not Acceptable | Serwer nie może spełnić Accept headers |
| **407** | Proxy Authentication Required | Wymagana autoryzacja proxy |
| **408** | Request Timeout | Timeout żądania |
| **409** | Conflict | Konflikt z aktualnym stanem serwera |
| **410** | Gone | Zasób już nie istnieje |
| **411** | Length Required | Wymagany Content-Length |
| **412** | Precondition Failed | Warunki wstępne nie zostały spełnione |
| **413** | Content Too Large | Payload zbyt duży |
| **414** | URI Too Long | URI zbyt długi |
| **415** | Unsupported Media Type | Nieobsługiwany typ mediów |
| **416** | Range Not Satisfiable | Zakres nie może być spełniony |
| **417** | Expectation Failed | Expect header nie może być spełniony |
| **418** | I'm a teapot | 🫖 Zabawny kod z RFC 2324 |
| **421** | Misdirected Request | Żądanie skierowane do złego serwera |
| **422** | Unprocessable Content | Błędy semantyczne w żądaniu |
| **423** | Locked | Zasób jest zablokowany |
| **424** | Failed Dependency | Zależność nie powiodła się |
| **425** | Too Early | Serwer nie chce przetwarzać żądania które może być powtórzone |
| **426** | Upgrade Required | Klient powinien przełączyć na inny protokół |
| **428** | Precondition Required | Serwer wymaga warunkowego żądania |
| **429** | Too Many Requests | Rate limiting - za dużo żądań |
| **431** | Request Header Fields Too Large | Nagłówki zbyt duże |
| **451** | Unavailable For Legal Reasons | Niedostępne z powodów prawnych |

### ⚠️ 5xx - Błędy Serwera (Server Errors)

| Kod | Nazwa | Opis |
|-----|-------|------|
| **500** | Internal Server Error | Ogólny błąd serwera |
| **501** | Not Implemented | Funkcja nie jest zaimplementowana |
| **502** | Bad Gateway | Nieprawidłowa odpowiedź od gateway |
| **503** | Service Unavailable | Serwer przeciążony/niedostępny |
| **504** | Gateway Timeout | Gateway timeout |
| **505** | HTTP Version Not Supported | Wersja HTTP nieobsługiwana |
| **506** | Variant Also Negotiates | Błąd konfiguracji serwera |
| **507** | Insufficient Storage | Brak miejsca na serwerze |
| **508** | Loop Detected | Wykryto nieskończoną pętlę |
| **510** | Not Extended | Wymagane dodatkowe rozszerzenia |
| **511** | Network Authentication Required | Wymagana autoryzacja sieciowa |

---

## 🚀 Podstawowe Użycie

### 1. Wysłanie prostego błędu

```cpp
#include "errors/error.hpp"

void handleError(int clientFD) {
    HttpError errorHandler;
    
    // Wygeneruj błąd 404 Not Found
    std::string errorResponse = errorHandler.generateErrorResponse(404);
    
    // Wyślij do klienta
    send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
}
```

### 2. Wysłanie błędu z niestandardowym komunikatem

```cpp
HttpError errorHandler;

std::string errorResponse = errorHandler.generateErrorResponse(500, 
    "Database connection failed. Please try again later.");
    
send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
```

### 3. Pobieranie opisu błędu

```cpp
HttpError errorHandler;

// Pobierz krótki opis
std::string message = errorHandler.getErrorMessage(403);
// Zwraca: "Forbidden"

// Pobierz pełny opis
std::string description = errorHandler.getErrorDescription(403);
// Zwraca: "The server refuses to authorize the request."
```

---

## 🔧 Zaawansowane Przykłady

### Walidacja pustego żądania HTTP (400)

```cpp
// void recivNClose(size_t el) {
//     char buffer[8192];
//     memset(buffer, 0, sizeof(buffer));
    
//     int byteRecived = recv(pollFDs[el].fd, buffer, sizeof(buffer) - 1, 0);
    
//     if (byteRecived <= 0) {
//         // Handle connection close or error
//         return;
//     }
    
//     buffer[byteRecived] = '\0';
//     std::string requestStr(buffer);
    
//     // ✅ WALIDACJA: Sprawdź czy request jest pusty
//     if (requestStr.empty() || requestStr.find("HTTP") == std::string::npos) {
//         HttpError errorHandler;
//         std::string errorResponse = errorHandler.generateErrorResponse(400, 
//             "Empty or malformed HTTP request.");
//         send(pollFDs[el].fd, errorResponse.c_str(), errorResponse.size(), 0);
//         close(pollFDs[el].fd);
//         return;
//     }
    
//     // Kontynuuj normalną obsługę...
// }
```

### Sprawdzanie rozmiaru payloadu (413)

```cpp
void checkPayloadSize(const std::string& requestStr, int clientFD) {
    const size_t MAX_PAYLOAD = 8192;
    
    size_t contentLength = 0;
    size_t pos = requestStr.find("Content-Length:");
    if (pos != std::string::npos) {
        // Parse Content-Length value
        contentLength = /* parse value */;
    }
    
    if (contentLength > MAX_PAYLOAD) {
        HttpError errorHandler;
        std::string errorResponse = errorHandler.generateErrorResponse(413,
            "Request payload exceeds maximum size of 8192 bytes.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
        close(clientFD);
        return;
    }
}
```

### Sprawdzanie metody HTTP (405, 501)

```cpp
void validateMethod(const std::string& method, int clientFD) {
    HttpError errorHandler;
    
    // Metody obsługiwane przez serwer
    if (method != "GET" && method != "POST" && method != "DELETE") {
        
        // Jeśli metoda jest poprawna ale nie zaimplementowana
        if (method == "PUT" || method == "PATCH") {
            std::string errorResponse = errorHandler.generateErrorResponse(501,
                "Method '" + method + "' is not implemented.");
            send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
            return;
        }
        
        // Jeśli metoda jest całkowicie nieprawidłowa
        std::string errorResponse = errorHandler.generateErrorResponse(405,
            "Method '" + method + "' is not allowed.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
        return;
    }
}
```

### Obsługa błędów CGI (502, 404, 403)

```cpp
void executeCgi(const std::string& scriptPath, int clientFD) {
    HttpError errorHandler;
    
    // Sprawdź czy skrypt istnieje
    if (access(scriptPath.c_str(), F_OK) != 0) {
        std::string errorResponse = errorHandler.generateErrorResponse(404,
            "CGI script '" + scriptPath + "' not found.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
        return;
    }
    
    // Sprawdź uprawnienia
    if (access(scriptPath.c_str(), X_OK) != 0) {
        std::string errorResponse = errorHandler.generateErrorResponse(403,
            "CGI script is not executable.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
        return;
    }
    
    // Wykonaj CGI
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execve(/* ... */);
        
        // Jeśli execve się nie powiedzie
        std::string errorResponse = errorHandler.generateErrorResponse(502,
            "Failed to execute CGI script.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
        exit(1);
    }
}
```

### Try-Catch z obsługą błędów

```cpp
void handleRequest(int clientFD) {
    HttpError errorHandler;
    
    try {
        // Twoja normalna logika serwera
        parseRequest();
        processRequest();
        sendResponse();
    }
    catch (const std::runtime_error& e) {
        std::cerr << "Runtime error: " << e.what() << std::endl;
        std::string errorResponse = errorHandler.generateErrorResponse(500,
            "Internal error: " + std::string(e.what()));
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        std::string errorResponse = errorHandler.generateErrorResponse(500);
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
    }
    catch (...) {
        std::cerr << "Unknown exception!" << std::endl;
        std::string errorResponse = errorHandler.generateErrorResponse(500,
            "An unknown error occurred.");
        send(clientFD, errorResponse.c_str(), errorResponse.size(), 0);
    }
}
```

---

## 🔗 Integracja z Serwerem

### Krok 1: Dodaj include do `poll.cpp`

```cpp
#include "errors/error.hpp"
```

### Krok 2: Zmień `recivNClose()` w `poll.cpp`

```cpp
void CoreEngine::recivNClose(size_t el) {
    memset(buffer, 0, sizeof(buffer));
    byteRecived = recv(pollFDs[el].fd, buffer, sizeof(buffer) - 1, 0);
    
    if (byteRecived == -1) {
        HttpError errorHandler;
        std::string errorResponse = errorHandler.generateErrorResponse(500);
        send(pollFDs[el].fd, errorResponse.c_str(), errorResponse.size(), 0);
        close(pollFDs[el].fd);
        return;
    }
    else if (byteRecived == 0) {
        // Close connection
        // ...
    }
    else {
        buffer[byteRecived] = '\0';
        std::string requestStr(buffer);
        
        // WALIDACJA
        if (requestStr.empty() || requestStr.find("HTTP") == std::string::npos) {
            HttpError errorHandler;
            std::string errorResponse = errorHandler.generateErrorResponse(400);
            send(pollFDs[el].fd, errorResponse.c_str(), errorResponse.size(), 0);
            close(pollFDs[el].fd);
            return;
        }
        
        pollFDs[el].events = POLLOUT;
    }
}
```

### Krok 3: Dodaj try-catch w `sendToClient()`

```cpp
void CoreEngine::sendToClient(size_t el) {
    HttpError errorHandler;
    
    try {
        std::string requestStr(buffer);
        Http response(requestStr, serversCfg[0]);
        std::string responseStr = response.responseBuilder();
        
        int byteSend = send(pollFDs[el].fd, responseStr.c_str(), responseStr.size(), 0);
        if (byteSend == -1) {
            std::string errorResponse = errorHandler.generateErrorResponse(500);
            send(pollFDs[el].fd, errorResponse.c_str(), errorResponse.size(), 0);
        }
    }
    catch (...) {
        std::string errorResponse = errorHandler.generateErrorResponse(500);
        send(pollFDs[el].fd, errorResponse.c_str(), errorResponse.size(), 0);
    }
    
    pollFDs[el].events = POLLIN;
}
```

---

## 🎨 Wygląd Stron Błędów

Każdy błąd generuje **piękną stronę HTML** z:

- ✅ Gradientowym tłem (fioletowy → różowy)
- ✅ Dużym kodem błędu z gradientem
- ✅ Czytelną nazwą błędu
- ✅ Szczegółowym opisem
- ✅ Przyciskiem "Go Home"
- ✅ Responsywnym designem

### Przykład wygenerowanego HTML:

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <title>404 Not Found</title>
    <style>
        body {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            /* ... piękny CSS ... */
        }
    </style>
</head>
<body>
    <div class="error-container">
        <div class="error-code">404</div>
        <div class="error-message">Not Found</div>
        <div class="error-description">
            The requested resource could not be found on this server.
        </div>
        <a href="/" class="back-button">Go Home</a>
    </div>
</body>
</html>
```

---

## 📦 Kompilacja

System błędów jest już dodany do Makefile:

```bash
make re     # Przekompiluj projekt
./webserv   # Uruchom serwer
```

---

## 🧪 Testowanie

### Test 1: Błąd 404

```bash
curl http://localhost:8080/nieistniejaca-strona
```

Powinieneś zobaczyć piękną stronę 404 w przeglądarce.

### Test 2: Pusty request (400)

```bash
echo "" | nc localhost 8080
```

### Test 3: Zbyt długie żądanie (413)

```bash
curl -X POST http://localhost:8080/ -d "$(python -c 'print("A"*10000)')"
```

### Test 4: Nieprawidłowa metoda (405)

```bash
curl -X PATCH http://localhost:8080/
```

---

## 📚 Źródła

System oparty na oficjalnej dokumentacji:
- 🌐 [MDN HTTP Status Codes](https://developer.mozilla.org/en-US/docs/Web/HTTP/Status)
- 📖 [RFC 7231 - HTTP/1.1 Semantics](https://tools.ietf.org/html/rfc7231)
- 📖 [RFC 7235 - HTTP/1.1 Authentication](https://tools.ietf.org/html/rfc7235)
