#include <QTest>
#include <qglobal.h>

// 每个测试文件提供一个运行入口，汇总到一个可执行文件中
int runLocalStorageTests(int argc, char *argv[]);
int runDocumentTests(int argc, char *argv[]);
int runDocumentManagerTests(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    const QByteArray only = qgetenv("QC_TEST_ONLY");
    if (only == "localstorage")
        return runLocalStorageTests(argc, argv);
    if (only == "document")
        return runDocumentTests(argc, argv);
    if (only == "manager")
        return runDocumentManagerTests(argc, argv);

    int failed = 0;
    failed += runLocalStorageTests(argc, argv);
    failed += runDocumentTests(argc, argv);
    failed += runDocumentManagerTests(argc, argv);
    return failed == 0 ? 0 : 1;
}
