-- The nem model as Apache Ossie YAML, in the lakehouse's Context folder as Fabric lays out semantic views (Context/Semantic Views/<view>/), over the lakehouse's Iceberg
-- catalog attached by hand. The YAML's datasets name mart.<table>, and its DirectLake expression
-- names no lakehouse yet ({WS_ID}, {LH_ID} are filled in at deploy), so the view reads its tables
-- from the search path: the catalog is attached and made current first. The view is created in
-- memory.main, not in the current schema, which is the catalog's and read-only. Search-path
-- tables are not pinned, so the view reads them as they are now and gets no orphan answers.
--
-- Run by CI (.github/scripts/onelake_sql.sh): ${...} is filled in from the environment, and the
-- first statement that fails, or check that calls error(), stops the run.

LOAD '${EXT}';
CREATE SECRET onelake (TYPE azure, PROVIDER access_token, ACCESS_TOKEN '${STORAGE_TOKEN}');
.echo off
ATTACH '${WS_ID}/${LH_ID}' AS nem_lake (TYPE iceberg, URI 'https://onelake.table.fabric.microsoft.com/iceberg',
    TOKEN '${STORAGE_TOKEN}', ACCESS_DELEGATION_MODE 'none');
.echo on

-- The lakehouse's tables first, then the view over them.
USE nem_lake.mart;
CREATE SEMANTIC VIEW memory.main.nem_yaml
    FROM 'abfss://${WS_ID}@onelake.dfs.fabric.microsoft.com/${LH_ID}/Context/Semantic Views/nem/nem.yaml';

SELECT view_name, path, tables_from, data_as_of FROM (SHOW SEMANTIC VIEWS);
SELECT count(*) AS metrics FROM (SHOW SEMANTIC METRICS IN nem_yaml);

-- Model queries.
SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw FROM nem_yaml GROUP BY ALL ORDER BY ALL;
SELECT dim_calendar.year, AGG("Curtailed MWh") AS curtailed, AGG("Curtailment rate") AS rate FROM nem_yaml GROUP BY ALL ORDER BY ALL;
SELECT dim_duid.FuelSourceDescriptor, AGG("Generation MWh") AS generation FROM nem_yaml GROUP BY ALL ORDER BY generation DESC NULLS LAST LIMIT 5;

-- A model query joined to a lakehouse table.
WITH g AS (SELECT dim_region.Region, round(AGG("Generation MWh"), 6) AS generation FROM nem_yaml GROUP BY ALL)
SELECT g.Region, generation, count(*) AS units FROM g JOIN nem_lake.mart.dim_duid d ON d."Region" = g.Region GROUP BY ALL ORDER BY 1;

-- Its answers equal plain SQL over the same tables.
SELECT CASE WHEN (SELECT AGG("Registered units") FROM nem_yaml) = (SELECT count(*) FROM nem_lake.mart.dim_duid)
    THEN 'Registered units: as plain SQL' ELSE error('Registered units differ from plain SQL') END AS "check";

WITH v AS (SELECT dim_duid.Region AS region, AGG("Registered MW") AS mw FROM nem_yaml GROUP BY ALL),
s AS (SELECT "Region" AS region, sum("RegCapMW")::DOUBLE AS mw FROM nem_lake.mart.dim_duid GROUP BY ALL HAVING sum("RegCapMW") IS NOT NULL)
SELECT CASE WHEN count(*) = 0 THEN 'Registered MW by region: as plain SQL'
    ELSE error('Registered MW by region differs from plain SQL in ' || count(*) || ' regions') END AS "check"
FROM v FULL JOIN s ON v.region IS NOT DISTINCT FROM s.region
WHERE v.mw IS NULL OR s.mw IS NULL OR abs(v.mw - s.mw) > 1e-9 * greatest(1, abs(s.mw));

SELECT CASE WHEN abs((SELECT AGG("Curtailed MWh") FROM nem_yaml) - (SELECT sum(curtailed_mwh)::DOUBLE FROM nem_lake.mart.fct_curtailment))
        <= 1e-9 * greatest(1, abs((SELECT sum(curtailed_mwh)::DOUBLE FROM nem_lake.mart.fct_curtailment)))
    THEN 'Curtailed MWh: as plain SQL' ELSE error('Curtailed MWh differs from plain SQL') END AS "check";

-- The same model as a Fabric semantic view's folder of YAML files (Context/Semantic Views/nem_split/:
-- datasets/dimensions.yaml, datasets/facts.yaml, relationships.yaml, metrics/all.yaml, model.yaml,
-- and a notes.yml that is not read). Every .yaml in it, at any depth, is merged into one model.
CREATE SEMANTIC VIEW memory.main.nem_folder
    FROM 'abfss://${WS_ID}@onelake.dfs.fabric.microsoft.com/${LH_ID}/Context/Semantic Views/nem_split';

SELECT view_name, path FROM (SHOW SEMANTIC VIEWS) WHERE view_name = 'nem_folder';

-- The folder's view is the file's: the same metrics and columns, and the same rows.
SELECT CASE WHEN (SELECT count(*) FROM ((SELECT * FROM (DESCRIBE nem_folder)) EXCEPT ALL (SELECT * FROM (DESCRIBE nem_yaml)))) = 0
     AND (SELECT count(*) FROM (SHOW SEMANTIC METRICS IN nem_folder)) = (SELECT count(*) FROM (SHOW SEMANTIC METRICS IN nem_yaml))
    THEN 'the folder: the same model as the file' ELSE error('the folder and the file differ') END AS "check";

WITH f AS (SELECT dim_region.Region, AGG("Registered units") AS units, round(AGG("Generation MWh"), 6) AS generation FROM nem_folder GROUP BY ALL),
y AS (SELECT dim_region.Region, AGG("Registered units") AS units, round(AGG("Generation MWh"), 6) AS generation FROM nem_yaml GROUP BY ALL)
SELECT CASE WHEN (SELECT count(*) FROM f) > 0
     AND (SELECT count(*) FROM (FROM f EXCEPT ALL FROM y)) = 0 AND (SELECT count(*) FROM (FROM y EXCEPT ALL FROM f)) = 0
    THEN 'the folder: the same rows as the file' ELSE error('the folder''s rows differ from the file''s') END AS "check";
