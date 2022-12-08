// Should xxx.html not exist these will be supplied instead.
#define DEFAULT_404_SITE "404 - Not Found"
#define DEFAULT_400_SITE "400 - Bad Request"
#define DEFAULT_500_SITE "500 - Internal Server Error"

// Default port should -p not be supplied.
#define DEFAULT_PORT 1337

// Default directory should -d not be supplied.
#define WEBSITE_ROOT "./site/"

#define TIMEOUT_SECOND 3
#define TIMEOUT_USECOND 0

#define HAS_PLEDGE (__OpenBSD__ | __serenity__)
#define HAS_UNVEIL (__OpenBSD__ | __serenity__)
#define HAS_SENDFILE (__linux__ | __FreeBSD__)
