/**
 * The comshu chat agent: three tools over one running `comshu ui`.
 *
 * Loaded into `pi` with `-e agent/comshu-agent.ts`. It registers `schema`, `sql`
 * and `chart` and nothing else — the sidecar is started with
 * `--no-builtin-tools`, so this agent has no shell, no filesystem and no way to
 * write anything anywhere. A chat pane that can answer questions about a
 * capture does not need to be able to touch the machine holding it.
 *
 * The capture is reached over HTTP at $COMSHU_UI_URL (a running `comshu ui`),
 * never by opening a file. That is what lets the agent run on a workstation
 * while the capture sits on a login node behind an SSH forward.
 */

import { Type } from "@earendil-works/pi-ai";
import { defineTool, type ExtensionAPI } from "@earendil-works/pi-coding-agent";

import { Capture, renderTable } from "./lib/query.mjs";
import { guardSpec, specFields } from "./lib/spec-guard.mjs";
import { tableCounts } from "./lib/preamble.mjs";

const capture = new Capture(process.env.COMSHU_UI_URL ?? "http://127.0.0.1:8777");

const err = (text: string) => ({
	content: [{ type: "text" as const, text }],
	details: undefined,
	isError: true,
});

const schemaTool = defineTool({
	name: "schema",
	label: "Schema",
	description:
		"List the capture's tables with row counts, or describe one table's columns and types. " +
		"Call with no arguments first if you are unsure what exists.",
	promptSnippet: "Inspect the capture's tables and columns",
	parameters: Type.Object({
		table: Type.Optional(
			Type.String({ description: "A table name. Omit to list every table with its row count." }),
		),
	}),

	async execute(_id, params) {
		try {
			if (!params.table) {
				const counts = await tableCounts(capture);
				const lines = counts
					.filter((c) => !c.table.startsWith("_"))
					.map((c) => `${c.table}  ${c.rows === 0 ? "(empty)" : `${c.rows} rows`}`);
				return {
					content: [{ type: "text", text: lines.join("\n") }],
					details: { tables: counts.length },
				};
			}
			if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(params.table)) return err(`not a table name: ${params.table}`);
			const cols = await capture.rows(
				`SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM "${params.table}")`,
			);
			if (!cols.length) return err(`no such table: ${params.table}`);
			const n = await capture.scalar(`SELECT count(*) FROM "${params.table}"`);
			const body = cols.map((c) => `  ${c.column_name} ${c.column_type}`).join("\n");
			return {
				content: [{ type: "text", text: `${params.table} — ${n} rows\n${body}` }],
				details: { table: params.table, columns: cols.length },
			};
		} catch (e) {
			return err(String((e as Error).message ?? e));
		}
	},
});

const sqlTool = defineTool({
	name: "sql",
	label: "SQL",
	description:
		"Run one read-only SELECT against the capture and see the result. Results are capped at 50 rows " +
		"(the true row count is reported when the cap bites), so aggregate in the query rather than " +
		"asking for raw rows. DuckDB syntax.",
	promptSnippet: "Query the capture with one SELECT",
	promptGuidelines: [
		"Answer any quantitative question about the capture by running sql, never from memory or inference.",
		"When sql returns a DuckDB error, read the error and correct the query — after three failures, say what could not be computed.",
	],
	parameters: Type.Object({
		query: Type.String({ description: "One SELECT or WITH statement. No semicolon." }),
	}),

	async execute(_id, params, _signal, onUpdate) {
		onUpdate?.({ content: [{ type: "text", text: "running…" }] });
		try {
			const result = await capture.modelQuery(params.query);
			return {
				content: [{ type: "text", text: renderTable(result) }],
				details: { rows: result.rows.length, total: result.total, truncated: result.truncated },
			};
		} catch (e) {
			return err(String((e as Error).message ?? e));
		}
	},
});

const chartTool = defineTool({
	name: "chart",
	label: "Chart",
	description:
		"Draw a result in the page. Give the query and a Vega-Lite spec with NO data in it: the page runs " +
		"the query itself and binds the rows under the name `rows`, so every `field` in the spec must be a " +
		"column that query returns. Use it when a distribution, ranking, matrix or time series answers the " +
		"question better than a sentence — one chart per answer.",
	promptSnippet: "Draw a query result as a Vega-Lite chart in the page",
	promptGuidelines: [
		"Use chart only after the same query has been run with sql and its columns are known.",
		"A chart spec passed to chart must not contain data, values, url or datasets — the sql field is the only data source.",
	],
	parameters: Type.Object({
		title: Type.String({ description: "A short title, in the user's terms." }),
		sql: Type.String({ description: "The SELECT that produces the rows the spec draws." }),
		spec: Type.Any({
			description:
				'Vega-Lite spec, e.g. {"mark":"bar","encoding":{"x":{"field":"dst_rank","type":"ordinal"},' +
				'"y":{"field":"bytes","type":"quantitative"}}}. No data key.',
		}),
	}),

	async execute(_id, params) {
		const guard = guardSpec(params.spec);
		if (!guard.ok) return err(`the spec was rejected:\n- ${guard.errors.join("\n- ")}`);

		// A chart whose spec names a column its query does not return renders
		// blank, and a blank chart looks like an empty capture rather than a
		// mistake. One capped run settles it before anyone sees the plot.
		let columns: string[];
		let total: number;
		try {
			const probe = await capture.modelQuery(params.sql, { limit: 1 });
			columns = probe.columns;
			total = probe.total;
		} catch (e) {
			return err(`the chart's query failed: ${String((e as Error).message ?? e)}`);
		}
		const missing = specFields(guard.spec).filter((f) => !columns.includes(f));
		if (missing.length) {
			return err(
				`the spec draws ${missing.map((m) => `\`${m}\``).join(", ")}, which the query does not return. ` +
					`Its columns are: ${columns.join(", ")}.`,
			);
		}
		if (total === 0) return err("the chart's query returns no rows — there is nothing to draw.");

		return {
			content: [
				{
					type: "text",
					text: `chart "${params.title}" is now shown to the user (${total} rows). Do not repeat its contents as a table.`,
				},
			],
			details: { chart: { title: params.title, sql: params.sql, spec: guard.spec, rows: total } },
		};
	},
});

export default function (pi: ExtensionAPI) {
	pi.registerTool(schemaTool);
	pi.registerTool(sqlTool);
	pi.registerTool(chartTool);
}
