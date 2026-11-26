-- Queries over the nem semantic view (the deployed nem model, Direct Lake), in the forms DuckDB
-- SQL can hold them. The extension must run each one, and rewrite each one to the SQL dax-sql
-- gives for it in Node (.github/scripts/drift.mjs). Whether the numbers are DAX's is dax-sql's to
-- test, not this repo's. ol is the same lakehouse, attached by the test.
-- One query a paragraph, each ending with a semicolon.

SELECT dim_region.Region, AGG("Generation MWh") AS generation, AGG("Revenue") AS revenue, AGG("Capture price") AS capture_price
FROM nem GROUP BY ALL ORDER BY Region;

SELECT dim_duid.FuelSourceDescriptor, AGG("Generation share") AS share, AGG("Renewable share") AS renewable
FROM nem GROUP BY ALL HAVING AGG("Generation MWh") > 0 ORDER BY FuelSourceDescriptor;

SELECT dim_time.hour, AGG("Generation MWh") AS generation, AGG("Units") AS units FROM nem GROUP BY ALL ORDER BY hour;

SELECT dim_region.Region, dim_duid.TechnologyType, AGG("Capacity factor") AS capacity_factor, GROUPING(dim_duid.TechnologyType) AS subtotal
FROM nem GROUP BY dim_region.Region, ROLLUP(dim_duid.TechnologyType) ORDER BY Region, TechnologyType;

SELECT dim_interconnector.interconnector, AGG("Flow MW") AS flow, AGG("Flow utilisation") AS utilisation
FROM nem GROUP BY ALL ORDER BY interconnector;

SELECT fct_region_hourly.hour, AGG("Price at hour") AS price FROM nem WHERE fct_region_hourly.REGIONID = 'NSW1' GROUP BY ALL ORDER BY hour;

SELECT dim_calendar.date, AGG("Generation MWh") AS generation, AGG("Emissions t") AS emissions
FROM nem WHERE dim_calendar.date >= DATE '2026-01-01' GROUP BY ALL ORDER BY date DESC LIMIT 7;

WITH g AS (SELECT dim_region.Region, AGG("Generation MWh") AS generation FROM nem GROUP BY ALL),
d AS (SELECT dim_region.Region, AGG("Demand MWh") AS demand FROM nem GROUP BY ALL)
SELECT g.Region, generation, demand, generation / NULLIF(demand, 0) AS ratio FROM g JOIN d USING (Region) ORDER BY 1;

SELECT StationName, generation, rank() OVER (ORDER BY generation DESC) AS place
FROM (SELECT dim_duid.StationName, AGG("Generation MWh") AS generation FROM nem GROUP BY ALL)
ORDER BY generation DESC NULLS LAST LIMIT 10;

SELECT r.Region, r.units, count(*) AS stations
FROM (SELECT dim_region.Region, AGG("Registered units") AS units FROM nem GROUP BY ALL) r
JOIN ol.mart.dim_duid d ON d."Region" = r.Region GROUP BY ALL ORDER BY 1;

(SELECT 'generation' AS what, AGG("Generation MWh") AS mwh FROM nem)
UNION ALL
(SELECT 'demand', AGG("Demand MWh") FROM nem);

SELECT count(*) AS big_regions FROM ol.mart.dim_duid
WHERE "Region" IN (SELECT dim_region.Region FROM nem GROUP BY ALL HAVING AGG("Registered MW") > 10000);

CREATE TEMP TABLE revenue_by_region AS SELECT dim_region.Region, AGG("Revenue") AS revenue FROM nem GROUP BY ALL;

SELECT * FROM revenue_by_region ORDER BY Region;

EXPLAIN SELECT dim_region.Region, AGG("Revenue") AS revenue FROM nem GROUP BY ALL;

SELECT 'FROM nem' AS text, (SELECT AGG("Registered units") FROM nem) AS units; -- FROM nem
