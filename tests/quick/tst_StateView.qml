import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "StateView"

    Component {
        id: stateViewComponent

        StateView {
            width: 320
            height: 180
        }
    }

    function test_loadingState() {
        const view = createTemporaryObject(stateViewComponent, this,
                                           { "viewState": StateView.Loading })
        verify(view)
        verify(view.loadingVisible)
        verify(!view.emptyVisible)
        verify(!view.errorVisible)
        compare(view.Accessible.name, "Loading")
    }

    function test_emptyState() {
        const view = createTemporaryObject(stateViewComponent, this,
                                           { "viewState": StateView.Empty,
                                             "emptyMessage": "No orders" })
        verify(view)
        verify(!view.loadingVisible)
        verify(view.emptyVisible)
        verify(!view.errorVisible)
        compare(view.Accessible.name, "No orders")
    }

    function test_errorState() {
        const view = createTemporaryObject(stateViewComponent, this,
                                           { "viewState": StateView.Error,
                                             "errorMessage": "Could not load orders" })
        verify(view)
        verify(!view.loadingVisible)
        verify(!view.emptyVisible)
        verify(view.errorVisible)
        compare(view.Accessible.name, "Could not load orders")
        compare(view.Accessible.description, "Error")
    }
}
