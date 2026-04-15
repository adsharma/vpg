#include <stdio.h>

void vpg_init(const char *data_dir, const char *username, const char *dbname);
const char *vpg_exec(const char *query);
const char *vpg_last_error_message(void);
void vpg_free(void *ptr);
void vpg_finish(void);

int
main(void)
{
	const char *result;

	vpg_init("./data", "postgres", "postgres");
	if (vpg_last_error_message() != NULL)
	{
		fprintf(stderr, "init error: %s\n", vpg_last_error_message());
		return 1;
	}

	result = vpg_exec("SELECT current_database(), current_user;");
	if (result == NULL)
	{
		fprintf(stderr, "exec error: %s\n", vpg_last_error_message());
		return 1;
	}

	printf("%s", result);
	vpg_free((void *) result);
	vpg_finish();
	return 0;
}
