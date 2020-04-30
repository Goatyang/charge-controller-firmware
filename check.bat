@ECHO OFF
:: This batch file performs the same checks as TravisCI and should be run after each commit and before
:: pushing the commits

echo "---------- Trailing whitespace check -------------"
REM git diff --check 'git rev-list --max-count=1 --reverse HEAD' ..$TRAVIS_BRANCH
git diff --check master
if errorlevel 1 (
    echo Failure Reason is %errorlevel%
    exit /b %errorlevel%
)

echo "---------- Running compile check -------------"
platformio run -e mppt-2420-lc-v0.10 -e mppt-1210-hus-v0.7 -e pwm-2420-lus-v0.3 -e pwm-2420-lus-v0.3-zephyr
if errorlevel 1 (
    echo Failure Reason is %errorlevel%
    exit /b %errorlevel%
)

echo "---------- Running unit-tests -------------"
platformio test -e unit-test-native
if errorlevel 1 (
    echo Failure Reason is %errorlevel%
    exit /b %errorlevel%
)

echo "---------- Running static code checks -------------"
platformio check -e mppt-1210-hus-v0.7 -e pwm-2420-lus-v0.3
if errorlevel 1 (
    echo Failure Reason is %errorlevel%
    exit /b %errorlevel%
)

echo "---------- All tests passed -------------"