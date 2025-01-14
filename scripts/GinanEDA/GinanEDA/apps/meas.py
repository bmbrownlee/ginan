import logging

import GinanEDA.apps.utilities as util
import numpy as np
import plotly.express as px
# Imports
import plotly.graph_objs as go
from app import app
from dash import dash_table, dcc, html
from dash.dependencies import Input, Output, State
from GinanEDA.datasets import db
from scipy.stats import normaltest, probplot
from statsmodels.graphics.gofplots import qqplot

logger = logging.getLogger(__name__)


def qq_plot(data, sample_size):
    d = (data - np.mean(data)) / np.std(data)
    qq = np.ones([sample_size, 2])
    np.random.shuffle(d)
    qq[:, 0] = np.sort(d[0:sample_size])
    qq[:, 1] = np.sort(np.random.normal(size=sample_size))
    return qq


possible_plot = ["Line", "Scatter", "Polar", "HistogramX", "HistogramY", "QQ"]
dropdown_type = html.Div(
    [
        dcc.Dropdown(
            id="mes_dropdown_type",
            options=[{"label": i, "value": i} for i in possible_plot],
            placeholder="Graph Type",
            value=None,
        )
    ],
    style={"width": "10%", "display": "inline-block"},
)


def check_list():
    return dcc.Checklist(
        id="aggregate",
        options=[
            {"label": "Aggregate (hist/stats)", "value": "agg"},
        ],
        value=[""],
    )


def exclude_start():
    return html.Div(
        [
            html.P(
                "Exclude the first points",
                style={"display": "inline-block", "margin-right": 5},
            ),
            dcc.Input(id="exclude_npt", value="0", type="text", size="2"),
            html.P(" points", style={"display": "inline-block", "margin-right": 5}),
        ]
    )


def dropdown_site(site_list):
    site_list2 = list(["ALL"])
    site_list2.extend(site_list)
    p = util.namedDropdown(
        None,
        id="mes_dropdown_site",
        options=[{"label": i, "value": i} for i in site_list2],
        placeholder="Site",
        value=None,
        multi=True,
    )
    p.style = {"width": "20%", "display": "inline-block"}
    return p


def dropdown_sat(sat_list):
    sat_list2 = list(["ALL"])
    sat_list2.extend(sat_list)
    p = util.namedDropdown(
        None,
        id="mes_dropdown_sat",
        options=[{"label": i, "value": i} for i in sat_list2],
        placeholder="Sat",
        value=None,
        multi=True,
    )
    p.style = {"width": "20%", "display": "inline-block"}
    return p


def keys(name):
    return [{"label": i, "value": i} for i in name if i not in db.exclude_measurements]


def dropdown_key_y(name):
    p = util.namedDropdown(
        None,
        id="mes_dropdown_key_y",
        options=[i for i in keys(name)],
        placeholder="Y Axis",
        value=None,
        multi=True,
    )
    p.style = {"width": "20%", "display": "inline-block"}
    return p


def dropdown_key_x(name):
    p = util.namedDropdown(
        None,
        id="mes_dropdown_key_x",
        options=[i for i in keys(name)],
        placeholder="X Axis",
        value=None,
    )
    p.style = {"width": "20%", "display": "inline-block"}
    return p


update_button = html.Div(
    [html.Button("update graph", id="update_graph", n_clicks=0)],
    style={"width": "5%", "display": "inline-block"},
)


def get_empty_graph(message):
    emptiness = {
        "layout": {
            "xaxis": {"visible": False},
            "yaxis": {
                "visible": False,
            },
            "annotations": [
                {
                    "text": message,
                    "xref": "paper",
                    "yref": "paper",
                    "showarrow": False,
                    "font": {"size": 28},
                }
            ],
        }
    }
    return emptiness


def generate_trace(graph_type, x, y, label):
    if graph_type == "Line" or graph_type == "Scatter":
        if graph_type == "Line":
            mode = "lines"
        else:
            mode = "markers"
        trace = go.Scatter(x=x, y=y, mode=mode, name=label)
    elif graph_type == "Polar":
        trace = go.Scatterpolar(r=y, theta=x, mode="markers", name=label)
    elif graph_type == "HistogramX":
        trace = go.Histogram(x=y, name=label)
    elif graph_type == "HistogramY":
        trace = go.Histogram(y=y, name=label)
    elif graph_type == "QQ":
        trace = go.Scatter(x=x, y=y, mode="Scatter")
    return trace


@app.callback(
    Output("plot1", "figure"),
    Output("tableDiv1", "children"),
    inputs=[Input("update_graph", "n_clicks")],
    state=[
        State("session-store", "data"),
        State("mes_dropdown_type", "value"),
        State("mes_dropdown_site", "value"),
        State("mes_dropdown_sat", "value"),
        State("mes_dropdown_key_x", "value"),
        State("mes_dropdown_key_y", "value"),
        State("mes_dropdown_site", "options"),
        State("mes_dropdown_sat", "options"),
        State("exclude_npt", "value"),
        State("aggregate", "value"),
    ],
)
def update_graph_measurements(
    click, data_dict, graph_type, site, sat, xaxis, yaxis, list_site, list_sat, exclude, aggr
):
    try:
        exclude = int(exclude)
    except:
        exclude = 0
    print(" ==== ", data_dict, click)
    table = []
    if exclude < 0:
        exclude = 0
    if (
        graph_type is None
        or site is None
        or sat is None
        or xaxis is None
        or yaxis is None
    ):
        return (
            get_empty_graph("Make sure a value for all the Dropdown Menu is selected"),
            [],
        )
    else:
        fig = go.Figure()
        site = [i["value"] for i in list_site] if "ALL" in site else site
        sat = [i["value"] for i in list_sat] if "ALL" in sat else sat
        logger.debug(
            f"\n Plotting request type {graph_type} \n\t\t sat {' ,'.join(sat)} \n\t\t site {' ,'.join(site)}"
        )
        trace = []
        tab = []
        for yaxis_ in yaxis:
            site_, sat_, x_, y_ = db.get_series(data_dict,
                "Measurements", None, site, sat, xaxis, yaxis_
            )
            if graph_type == "QQ":
                if "agg" in aggr:
                    _y = []
                    for y in y_:
                        _y.append(y[exclude:])  # .ravel()
                    _y = np.concatenate(_y, axis=0)
                    qqplot_data = qq_plot(_y, len(_y))

                    trace.append(
                        generate_trace(
                            "Scatter", qqplot_data[:, 0], qqplot_data[:, 1], f"Agg"
                        )
                    )
                    trace.append(
                        generate_trace(
                            "Line",
                            [qqplot_data[0, 0], qqplot_data[-1, 0]],
                            [qqplot_data[0, 0], qqplot_data[-1, 0]],
                            f"Agg",
                        )
                    )
                else:
                    for i in range(len(x_)):
                        qqplot_data = qq_plot(y_[i][exclude:], len(y_[i][exclude:]))
                        trace.append(
                            generate_trace(
                                "Scatter",
                                qqplot_data[:, 0],
                                qqplot_data[:, 1],
                                f"{site_[i]} {sat_[i]}",
                            )
                        )
            elif "agg" in aggr and graph_type in ["HistogramX", "HistogramY"]:
                _y = []
                for y in y_:
                    _y.append(y[exclude:])  # .ravel()
                print(_y)
                _y = np.concatenate(_y, axis=0)
                print(_y)
                trace.append(
                    generate_trace(graph_type, x_[0][exclude:], _y, f"{yaxis_}")
                )
                a = dict()
                a["ID"] = f"{yaxis_}"
                a["RMS"] = np.sqrt(np.mean(np.square(_y)))
                a["Mean"] = np.mean(_y)
                a["Std"] = np.std(_y)
                print(a)
                table.append(a)

            else:
                for i in range(len(x_)):
                    logging.debug(f"ploting data {site_[i]} {sat_[i]}")
                    _y = y_[i][exclude:]
                    label = []
                    if len(site) != 1:
                        label.append(site_[i])
                    if len(sat) != 1:
                        label.append(sat_[i])
                    if len(yaxis) != 1:
                        label.append(yaxis_)
                    trace.append(
                        generate_trace(
                            graph_type, x_[i][exclude:], _y, f"{'-'.join(label)}"
                        )
                    )
                    if "agg" not in aggr:
                        if np.issubdtype(_y.dtype, np.float):
                            a = dict()
                            a["ID"] = f"{'-'.join(label)}"
                            a["RMS"] = np.sqrt(np.mean(np.square(_y)))
                            a["Mean"] = np.mean(_y)
                            a["Std"] = np.std(_y)
                            table.append(a)
                if "agg" in aggr and np.issubdtype(y_[0].dtype, np.float):
                    _y = []
                    for y in y_:
                        _y.append(y[exclude:])  # .ravel()
                    _y = np.concatenate(_y, axis=0)

                    a = dict()
                    a["ID"] = f"{yaxis_}"
                    a["RMS"] = np.sqrt(np.mean(np.square(_y)))
                    a["Mean"] = np.mean(_y)
                    a["Std"] = np.std(_y)
                    table.append(a)


        fig = go.Figure(data=trace)
        cols = []
        cols.append("ID")
        cols.append("RMS")
        cols.append("Mean")
        cols.append("Std")
        tab = [
            dash_table.DataTable(
                id="table",
                columns=[{"name": i, "id": i} for i in cols],
                data=table,
            )
        ]
        if graph_type == "QQ":
            fig.update_layout(
                xaxis=dict(
                    rangeslider=dict(visible=True), scaleanchor="y", scaleratio=1
                ),
                yaxis=dict(fixedrange=False, tickformat=".3f"),
                height=700,
                width=700,
            )
        else:
            fig.update_layout(
                xaxis=dict(rangeslider=dict(visible=True)),
                yaxis=dict(fixedrange=False, tickformat=".3f"),
                height=600,
            )
        fig.layout.autosize = True
        if graph_type == "Polar":
            if yaxis == "Elevation":
                fig.update_polars(radialaxis_range=[90, 0])
            if xaxis == "Azimuth":
                fig.update_polars(angularaxis_direction="clockwise")
    return fig, tab


def layout(data_dict):
    if data_dict== None:
        return html.Div(
            [html.P("First you will need to select a DB in the Db Info menu")]
        )
    else:
        return html.Div(
            [
                dropdown_type,
                dropdown_site(data_dict['DB_SITE']),
                dropdown_sat(data_dict['DB_SAT']),
                dropdown_key_x(data_dict['DB_MEAS_KEY']),
                dropdown_key_y(data_dict['DB_MEAS_KEY']),
                exclude_start(),
                check_list(),
                update_button,
                dcc.Graph(
                    id="plot1", figure=get_empty_graph("select information first")
                ),
                html.Div(id="tableDiv1", className="tableDiv"),
            ]
        )
