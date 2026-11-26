-- Queries over views read from the other formats, for the drift check: aac_ossie (aac's
-- ossie.yaml) and contoso_tmdl (contoso written as one .tmdl). One query a paragraph.

SELECT dim_duid.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw FROM aac_ossie GROUP BY ALL ORDER BY ALL;

SELECT dim_duid.FuelSourceDescriptor, AGG("Generation MWh") AS mwh FROM aac_ossie GROUP BY ALL ORDER BY ALL;

SELECT dim_calendar.date, AGG("Curtailed MWh") AS curtailed, AGG("Curtailment rate") AS rate FROM aac_ossie GROUP BY ALL ORDER BY ALL;

SELECT Category.Category, Product.Color, AGG("Sales Amount") AS amount, AGG("Share of Category") AS share
FROM contoso_tmdl GROUP BY ALL ORDER BY Category, Color;

SELECT "Date".Year, AGG("Sales YTD") AS ytd, AGG("Sales PY") AS py FROM contoso_tmdl GROUP BY ALL ORDER BY Year;
