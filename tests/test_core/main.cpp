#include <QTest>
#include <qglobal.h>

// 每个测试文件提供一个运行入口，汇总到一个可执行文件中
int runLocalStorageTests(int argc, char *argv[]);
int runDocumentTests(int argc, char *argv[]);
int runDocumentManagerTests(int argc, char *argv[]);
int runMultiCursorEngineTests(int argc, char *argv[]);
int runSessionManagerTests(int argc, char *argv[]);
int runShellIntegrationTests(int argc, char *argv[]);
int runSingleInstanceTests(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    const QByteArray only = qgetenv("QC_TEST_ONLY");
    if (only == "localstorage")
        return runLocalStorageTests(argc, argv);
    if (only == "document")
        return runDocumentTests(argc, argv);
    if (only == "manager")
        return runDocumentManagerTests(argc, argv);
    if (only == "mcengine")
        return runMultiCursorEngineTests(argc, argv);
    if (only == "session")
        return runSessionManagerTests(argc, argv);
    if (only == "shell")
        return runShellIntegrationTests(argc, argv);
    if (only == "singleinstance")
        return runSingleInstanceTests(argc, argv);

    int failed = 0;
    failed += runLocalStorageTests(argc, argv);
    failed += runDocumentTests(argc, argv);
    failed += runDocumentManagerTests(argc, argv);
    failed += runMultiCursorEngineTests(argc, argv);
    failed += runSessionManagerTests(argc, argv);
    failed += runShellIntegrationTests(argc, argv);
    failed += runSingleInstanceTests(argc, argv);
    return failed == 0 ? 0 : 1;
}
