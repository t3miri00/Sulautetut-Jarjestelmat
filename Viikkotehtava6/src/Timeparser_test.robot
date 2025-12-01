*** Settings ***
Library           RPA.Serial

*** Variables ***
${PORT}           COM3
${BAUD}           115200
${TIMEOUT}        3 seconds

*** Test Cases ***

Valid Time Should Return Seconds
    Open Serial Port    ${PORT}    baudrate=${BAUD}    timeout=${TIMEOUT}
    Write Serial    000120\n
    ${resp}=        Read Until    seconds=2
    Should Contain  ${resp}    80
    Close Serial Port

Invalid Seconds Should Return Error
    Open Serial Port    ${PORT}    baudrate=${BAUD}    timeout=${TIMEOUT}
    Write Serial    001067\n
    ${resp}=        Read Until    seconds=2
    Should Contain  ${resp}    -6
    Close Serial Port

Zero Time Should Return Error
    Open Serial Port    ${PORT}    baudrate=${BAUD}    timeout=${TIMEOUT}
    Write Serial    000000\n
    ${resp}=        Read Until    seconds=2
    Should Contain  ${resp}    -7
    Close Serial Port

NonNumeric Should Return Error
    Open Serial Port    ${PORT}    baudrate=${BAUD}    timeout=${TIMEOUT}
    Write Serial    12AB56\n
    ${resp}=        Read Until    seconds=2
    Should Contain  ${resp}    -3
    Close Serial Port