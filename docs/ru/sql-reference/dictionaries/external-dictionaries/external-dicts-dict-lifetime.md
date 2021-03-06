---
toc_priority: 42
toc_title: "Обновление словарей"
---

# Обновление словарей {#obnovlenie-slovarei}

ClickHouse периодически обновляет словари. Интервал обновления для полностью загружаемых словарей и интервал инвалидации для кэшируемых словарей определяется в теге `<lifetime>` в секундах.

Обновление словарей (кроме загрузки при первом использовании) не блокирует запросы - во время обновления используется старая версия словаря. Если при обновлении возникнет ошибка, то ошибка пишется в лог сервера, а запросы продолжат использовать старую версию словарей.

Пример настройки:

``` xml
<dictionary>
    ...
    <lifetime>300</lifetime>
    ...
</dictionary>
```

или

``` sql
CREATE DICTIONARY (...)
...
LIFETIME(300)
...
```

Настройка `<lifetime>0</lifetime>` (`LIFETIME(0)`) запрещает обновление словарей.

Можно задать интервал, внутри которого ClickHouse равномерно-случайно выберет время для обновления. Это необходимо для распределения нагрузки на источник словаря при обновлении на большом количестве серверов.

Пример настройки:

``` xml
<dictionary>
    ...
    <lifetime>
        <min>300</min>
        <max>360</max>
    </lifetime>
    ...
</dictionary>
```

или

``` sql
LIFETIME(MIN 300 MAX 360)
```

Если `<min>0</min>` и `<max>0</max>`, ClickHouse не перегружает словарь по истечению времени.
В этм случае, ClickHouse может перезагрузить данные словаря если изменился XML файл с конфигурацией словаря или если была выполнена команда `SYSTEM RELOAD DICTIONARY`.

При обновлении словарей сервер ClickHouse применяет различную логику в зависимости от типа [источника](external-dicts-dict-sources.md):

-   У текстового файла проверяется время модификации. Если время изменилось по отношению к запомненному ранее, то словарь обновляется.
-   Для MySQL источника, время модификации проверяется запросом `SHOW TABLE STATUS` (для MySQL 8 необходимо отключить кеширование мета-информации в MySQL `set global information_schema_stats_expiry=0`.
-   Словари из других источников по умолчанию обновляются каждый раз.

Для других источников (ODBC, PostgreSQL, ClickHouse и т.д.) можно настроить запрос, который позволит обновлять словари только в случае их фактического изменения, а не каждый раз. Чтобы это сделать необходимо выполнить следующие условия/действия:

-   В таблице словаря должно быть поле, которое гарантированно изменяется при обновлении данных в источнике.
-   В настройках источника указывается запрос, который получает изменяющееся поле. Результат запроса сервер ClickHouse интерпретирует как строку и если эта строка изменилась по отношению к предыдущему состоянию, то словарь обновляется. Запрос следует указывать в поле `<invalidate_query>` настроек [источника](external-dicts-dict-sources.md).

Пример настройки:

``` xml
<dictionary>
    ...
    <odbc>
      ...
      <invalidate_query>SELECT update_time FROM dictionary_source where id = 1</invalidate_query>
    </odbc>
    ...
</dictionary>
```

или

``` sql
...
SOURCE(ODBC(... invalidate_query 'SELECT update_time FROM dictionary_source where id = 1'))
...
```

Для словарей `Cache`, `ComplexKeyCache`, `SSDCache` и `SSDComplexKeyCache` поддерживается как синхронное, так и асинхронное обновление.
